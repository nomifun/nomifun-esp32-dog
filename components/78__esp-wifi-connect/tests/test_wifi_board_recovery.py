"""Exercise exact board startup and station event methods with native MSVC.

No sockets, USB access, Wi-Fi adapters, clocks, or NVS device access are used.
The fake event-group wait delivers association/DHCP/disconnect events to the
real station handlers, so association cannot masquerade as an IP connection.
"""

import os
from pathlib import Path
import subprocess
import tempfile

import test_saved_add as host


PRELUDE = r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING 0
#define WIFI_EVENT_CONNECTED 1u
#define WIFI_EVENT_STOPPED 2u
#define WIFI_EVENT_SCAN_DONE_BIT 4u
#define MAX_RECONNECT_COUNT 5
constexpr int pdFALSE = 0;
constexpr int ESP_OK = 0;
constexpr int portTICK_PERIOD_MS = 10;
constexpr int WIFI_REASON_AUTH_EXPIRE = 2;
constexpr int WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15;
constexpr int WIFI_REASON_NO_AP_FOUND = 201;
constexpr int WIFI_REASON_AUTH_FAIL = 202;
constexpr int WIFI_REASON_ASSOC_FAIL = 203;
constexpr int WIFI_REASON_HANDSHAKE_TIMEOUT = 204;
constexpr int WIFI_REASON_CONNECTION_FAIL = 205;
constexpr int WIFI_EVENT_STA_START = 1;
constexpr int WIFI_EVENT_SCAN_DONE = 2;
constexpr int WIFI_EVENT_STA_DISCONNECTED = 3;
constexpr int WIFI_EVENT_STA_CONNECTED = 4;
using esp_event_base_t = const char*;
struct wifi_event_sta_disconnected_t { uint8_t reason; };
struct ip_event_got_ip_t { struct { struct { uint32_t addr; } ip; } ip_info; };
template<class T> void esp_ip4addr_ntoa(T*, char* out, size_t length) {
    std::strncpy(out, "192.168.1.42", length);
}
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::vector<std::string> events;
int cases = 0;
int ap_entries = 0;
std::map<std::string, std::string> nvs;
struct PortalReached {};
unsigned pdMS_TO_TICKS(unsigned ms) { return ms / portTICK_PERIOD_MS; }
void vTaskDelay(unsigned ticks) {
    events.push_back("delay:" + std::to_string(ticks));
    if (ticks == pdMS_TO_TICKS(10000)) throw PortalReached{};
}
void esp_restart() { events.push_back("restart"); }
struct Display {
    std::vector<std::string> notices;
    std::string chat;
    void ShowNotification(const char* text, int) { notices.push_back(text); }
    void SetChatMessage(const char*, const char* text) { chat = text; }
};
struct Board {
    Display display;
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
};
enum DeviceState { kDeviceStateStarting, kDeviceStateWifiConfiguring };
struct Application {
    DeviceState state = kDeviceStateStarting;
    std::string alert;
    static Application& GetInstance() { static Application application; return application; }
    void SetDeviceState(DeviceState value) { state = value; events.push_back("state:configuring"); }
    void Alert(const char*, const char* text, const char*, const char*) { alert = text; }
};
namespace Lang {
const char* CODE = "zh-CN";
namespace Strings {
const char* CONNECT_TO_HOTSPOT = "Connect hotspot ";
const char* ACCESS_VIA_BROWSER = " at ";
const char* WIFI_CONFIG_MODE = "Provisioning";
const char* SCANNING_WIFI = "Scanning";
const char* CONNECT_TO = "Connecting ";
const char* CONNECTED_TO = "Connected ";
}
namespace Sounds { const char* OGG_WIFICONFIG = "sound"; }
}
struct SsidManager {
    std::vector<std::string> ssids;
    static SsidManager& GetInstance() { static SsidManager manager; return manager; }
    std::vector<std::string> GetSsidList() { return ssids; }
    bool AddSsid(const std::string& ssid, const std::string& password) {
        nvs["wifi:ssid"] = ssid; nvs["wifi:password"] = password; return true;
    }
    void Clear() { ssids.clear(); nvs.clear(); }
};
struct EventGroup { unsigned bits = 0; };
using EventGroupHandle_t = EventGroup*;
unsigned xEventGroupSetBits(EventGroupHandle_t group, unsigned bits) { return group->bits |= bits; }
unsigned xEventGroupClearBits(EventGroupHandle_t group, unsigned bits) { group->bits &= ~bits; return group->bits; }
unsigned xEventGroupGetBits(EventGroupHandle_t group) { return group->bits; }
unsigned xEventGroupWaitBits(EventGroupHandle_t, unsigned, int, int, unsigned);
int esp_wifi_scan_start(void*, bool) { events.push_back("scan"); return ESP_OK; }
int esp_wifi_connect() { events.push_back("retry"); return ESP_OK; }
int esp_timer_start_once(void*, int) { events.push_back("timer"); return ESP_OK; }
const char* esp_err_to_name(int) { return "injected driver error"; }
struct WifiStation {
    EventGroup group;
    EventGroupHandle_t event_group_ = &group;
    void* timer_handle_ = nullptr;
    std::string ssid_;
    std::string ip_address_;
    bool was_connected_ = false;
    bool direct_connect_attempted_ = false;
    int reconnect_count_ = 0;
    int scan_current_interval_microseconds_ = 10000000;
    int scan_min_interval_microseconds_ = 10000000;
    std::atomic<int> last_disconnect_reason_{0};
    std::vector<int> connect_queue_;
    std::function<void()> on_scan_begin_;
    std::function<void(const std::string&)> on_connect_;
    std::function<void(const std::string&)> on_connected_;
    std::function<void(int)> on_disconnected_;
    int starts = 0;
    int stops = 0;
    int waits = 0;
    unsigned wait_ticks = 0;
    bool associate = true;
    bool dhcp = false;
    bool stop_during_wait = false;
    int disconnect_reason = 0;
    bool find_ap = true;
    void OnScanBegin(std::function<void()> value) { on_scan_begin_ = value; events.push_back("register:scan"); }
    void OnConnect(std::function<void(const std::string&)> value) { on_connect_ = value; events.push_back("register:connect"); }
    void OnConnected(std::function<void(const std::string&)> value) { on_connected_ = value; events.push_back("register:connected"); }
    void Start() {
        check(on_scan_begin_ && on_connect_ && on_connected_, "Station started without callbacks");
        ++starts; events.push_back("station:start"); group.bits = 0;
        last_disconnect_reason_.store(0);
        ssid_ = find_ap ? "Saved Home" : "";
        on_scan_begin_();
        if (find_ap) on_connect_(ssid_);
    }
    void Stop() {
        ++stops; events.push_back("station:stop");
        group.bits = WIFI_EVENT_STOPPED;
        was_connected_ = false;
        // Production Stop unregisters event handlers and retains the attempted
        // SSID/reason. test_wifi_station.py verifies that real Stop contract.
    }
    bool WaitForConnected(int timeout_ms);
    bool IsConnected();
    int GetLastDisconnectReason() const {
        events.push_back("station:read_reason"); return last_disconnect_reason_.load();
    }
    std::string GetSsid() const { events.push_back("station:read_ssid"); return ssid_; }
    void HandleScanResult() { events.push_back("scan:result"); }
    void StartScan();
    void ScheduleNextScan();
    void StartConnect() { events.push_back("connect:next"); }
    void UpdateScanInterval() { events.push_back("scan:interval"); }
    static void WifiEventHandler(void*, esp_event_base_t, int32_t, void*);
    static void IpEventHandler(void*, esp_event_base_t, int32_t, void*);
};
WifiStation station;
WifiStation& GetWifiStation() { return station; }
unsigned xEventGroupWaitBits(EventGroupHandle_t group, unsigned mask, int clear, int all, unsigned ticks) {
    ++station.waits; station.wait_ticks = ticks; events.push_back("station:wait");
    check(mask == (WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED), "Wait does not observe DHCP/stopped bits");
    check(clear == pdFALSE && all == pdFALSE, "Wait changed event-group semantics");
    if (station.associate) {
        WifiStation::WifiEventHandler(&station, "WIFI", WIFI_EVENT_STA_CONNECTED, nullptr);
        check(!(group->bits & WIFI_EVENT_CONNECTED), "Association incorrectly signalled DHCP connection");
    }
    if (station.disconnect_reason) {
        wifi_event_sta_disconnected_t event{static_cast<uint8_t>(station.disconnect_reason)};
        WifiStation::WifiEventHandler(&station, "WIFI", WIFI_EVENT_STA_DISCONNECTED, &event);
    }
    if (station.dhcp) {
        ip_event_got_ip_t event{};
        WifiStation::IpEventHandler(&station, "IP", 0, &event);
    }
    if (station.stop_during_wait) xEventGroupSetBits(group, WIFI_EVENT_STOPPED);
    return group->bits;
}
struct WifiConfigurationAp {
    std::string failure;
    int starts = 0;
    std::function<void()> exit_callback;
    void SetLanguage(const char*) {}
    void SetSsidPrefix(const char*) {}
    void SetConnectionFailure(const std::string& value) { failure = value; events.push_back("ap:failure"); }
    void OnExitRequested(std::function<void()> value) { exit_callback = value; }
    void Start() {
        check(static_cast<bool>(exit_callback), "AP started without reboot callback");
        ++starts; ++ap_entries; events.push_back("ap:start");
    }
    std::string GetSsid() { return "Xiaozhi-Test"; }
    std::string GetWebServerUrl() { return "http://192.168.4.1"; }
};
WifiConfigurationAp ap;
WifiConfigurationAp& GetWifiAp() { return ap; }
struct WifiBoard {
    bool wifi_config_mode_ = false;
    std::string wifi_connection_failure_;
    void StartNetwork();
    void EnterWifiConfigMode();
};
void reset() {
    events.clear(); ap_entries = 0; ap = WifiConfigurationAp{};
    // Atomic fields make assignment intentionally unavailable.
    station.~WifiStation(); new (&station) WifiStation{};
    SsidManager::GetInstance().ssids = {"Saved Home"};
    nvs = {{"wifi:ssid", "Saved Home"}, {"wifi:password", "test-secret"},
           {"wifi:ota_url", "https://ota.example/firmware"}, {"petdog:angle_settings", "retained"}};
    Application::GetInstance() = Application{};
    Board::GetInstance() = Board{};
}
size_t event_index(const std::string& name) {
    auto found = std::find(events.begin(), events.end(), name);
    check(found != events.end(), "Expected event missing");
    return static_cast<size_t>(found - events.begin());
}
void run(WifiBoard& board) {
    const auto before = nvs;
    const auto ssids_before = SsidManager::GetInstance().ssids;
    try { board.StartNetwork(); } catch (const PortalReached&) {}
    check(nvs == before && SsidManager::GetInstance().ssids == ssids_before,
          "Startup recovery modified saved SSIDs, OTA URL, password or calibration");
}
void check_recovery(WifiBoard& board, const std::string& expected, int reason) {
    run(board);
    check(station.starts == 1 && station.waits == 1 && station.stops == 1, "Failed station not waited/stopped exactly once");
    check(station.wait_ticks == 60000 / portTICK_PERIOD_MS, "Board does not wait 60 seconds for DHCP");
    check(board.wifi_config_mode_ && ap_entries == 1 && ap.starts == 1, "Failure did not return to provisioning");
    check(event_index("station:wait") < event_index("station:stop") &&
          event_index("station:stop") < event_index("ap:start"), "AP started before station stopped");
    check(event_index("station:stop") < event_index("station:read_reason") &&
          event_index("station:stop") < event_index("station:read_ssid"),
          "Recovery read reason/SSID before station callbacks were stopped");
    check(event_index("ap:failure") < event_index("ap:start"), "Failure reason set after AP start");
    check(board.wifi_connection_failure_.find(expected) != std::string::npos, "Wrong failure classification");
    check(ap.failure == board.wifi_connection_failure_ && Board::GetInstance().display.chat == ap.failure,
          "Failure reason not passed to AP and display");
    if (reason) check(ap.failure.find(std::to_string(reason)) != std::string::npos, "Actual disconnect reason missing");
    check(Application::GetInstance().state == kDeviceStateWifiConfiguring, "Recovery state not configuring");
    check(Application::GetInstance().alert.find("Xiaozhi-Test") != std::string::npos, "Recovery did not show hotspot");
    ++cases;
}
'''

CASES = r'''
int run_cases() {
    reset(); WifiBoard no_ssids; SsidManager::GetInstance().ssids.clear(); run(no_ssids);
    check(no_ssids.wifi_config_mode_ && ap_entries == 1 && !station.starts && !station.waits && !station.stops,
          "Empty saved list did not go directly to AP");
    check(ap.failure.empty(), "First-time provisioning has a false failure"); ++cases;

    reset(); WifiBoard forced; forced.wifi_config_mode_ = true; run(forced);
    check(ap_entries == 1 && !station.starts && !station.waits && !station.stops,
          "force_ap did not bypass station while preserving saved settings"); ++cases;

    reset(); WifiBoard successful; station.dhcp = true; run(successful);
    check(station.starts == 1 && station.waits == 1 && !station.stops && !ap_entries,
          "DHCP success incorrectly returned to AP");
    check(station.wait_ticks == 60000 / portTICK_PERIOD_MS && station.IsConnected(), "DHCP success did not set connected state");
    check(station.ip_address_ == "192.168.1.42" && station.was_connected_, "Real IP handler not run");
    check(Board::GetInstance().display.notices.back() == "Connected Saved Home", "Connected notification not delayed until DHCP"); ++cases;

    reset(); WifiBoard association_only; check_recovery(association_only, "IP", 0);
    check(Board::GetInstance().display.notices.back() == "Connecting Saved Home...", "Association falsely displayed connected");

    reset(); WifiBoard no_scan_result; station.associate = false; station.find_ap = false;
    check_recovery(no_scan_result, "未找到", 0);

    reset(); WifiBoard missing_ap; station.disconnect_reason = WIFI_REASON_NO_AP_FOUND;
    check_recovery(missing_ap, "未找到", WIFI_REASON_NO_AP_FOUND);

    for (int reason : {WIFI_REASON_AUTH_FAIL, WIFI_REASON_AUTH_EXPIRE,
                       WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, WIFI_REASON_HANDSHAKE_TIMEOUT}) {
        reset(); WifiBoard authentication; station.disconnect_reason = reason;
        check_recovery(authentication, "认证或握手失败", reason);
    }
    for (int reason : {WIFI_REASON_ASSOC_FAIL, WIFI_REASON_CONNECTION_FAIL}) {
        reset(); WifiBoard association_failed; station.disconnect_reason = reason;
        check_recovery(association_failed, "路由器连接失败", reason);
    }
    for (int reason : {200, 211}) {
        reset(); WifiBoard other_reason; station.disconnect_reason = reason;
        check_recovery(other_reason, "连接中断", reason);
    }
    reset(); WifiBoard stopped; station.associate = false; station.stop_during_wait = true;
    check_recovery(stopped, "IP", 0);

    // An earlier failure followed by DHCP must clear the stale reason and succeed.
    reset(); WifiBoard recovered; station.disconnect_reason = WIFI_REASON_AUTH_FAIL; station.dhcp = true;
    run(recovered);
    check(station.last_disconnect_reason_.load() == 0 && station.IsConnected() && !ap_entries && !station.stops,
          "DHCP recovery retained an old reason or returned to AP"); ++cases;

    // The production station callback must revoke connected status on disconnect.
    wifi_event_sta_disconnected_t disconnected{WIFI_REASON_AUTH_FAIL};
    WifiStation::WifiEventHandler(&station, "WIFI", WIFI_EVENT_STA_DISCONNECTED, &disconnected);
    check(!station.IsConnected() && !station.was_connected_ && station.GetLastDisconnectReason() == WIFI_REASON_AUTH_FAIL,
          "Disconnect did not clear DHCP-connected state and record reason"); ++cases;

    // AP exit is deferred by the HTTP layer; the board supplies the restart action.
    reset(); WifiBoard initial; initial.wifi_config_mode_ = true; run(initial);
    check(std::find(events.begin(), events.end(), "restart") == events.end(), "Entering AP rebooted prematurely");
    ap.exit_callback(); check(events.back() == "restart", "AP exit callback did not restart board"); ++cases;

    std::cout << cases << " WiFi board startup/DHCP/recovery cases passed\n";
    return 0;
}
int main() {
    try {
        return run_cases();
    } catch (const std::exception& error) {
        std::cerr << "WiFi board recovery assertion failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "WiFi board recovery test raised an unknown exception\n";
        return 1;
    }
}
'''


def extract_function(source, signature):
    """Copy an entire production function without replacing its body."""
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    cursor = brace + 1
    while depth:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    return source[start:cursor] + "\n"


def main():
    firmware = host.COMPONENT.parents[1]
    board = (firmware / "main/boards/common/wifi_board.cc").read_text(encoding="utf-8")
    station = (host.COMPONENT / "wifi_station.cc").read_text(encoding="utf-8")
    body = PRELUDE
    for signature in ("bool WifiStation::WaitForConnected(", "bool WifiStation::IsConnected(",
                      "void WifiStation::StartScan(", "void WifiStation::ScheduleNextScan(",
                      "void WifiStation::WifiEventHandler(", "void WifiStation::IpEventHandler("):
        body += extract_function(station, signature)
    for signature in ("void WifiBoard::EnterWifiConfigMode(", "void WifiBoard::StartNetwork("):
        body += extract_function(board, signature)
    body += CASES
    compiler_bin = host.VC / "bin/Hostx64/x64"
    env = os.environ.copy()
    env["PATH"] = str(compiler_bin) + os.pathsep + env["PATH"]
    env["INCLUDE"] = os.pathsep.join(str(path) for path in
        (host.VC / "include", host.SDK / "Include" / host.SDK_VERSION / "ucrt",
         host.SDK / "Include" / host.SDK_VERSION / "shared", host.SDK / "Include" / host.SDK_VERSION / "um"))
    env["LIB"] = os.pathsep.join(str(path) for path in
        (host.VC / "lib/x64", host.SDK / "Lib" / host.SDK_VERSION / "ucrt/x64",
         host.SDK / "Lib" / host.SDK_VERSION / "um/x64"))
    with tempfile.TemporaryDirectory(prefix="wifi-board-host-", dir=Path(__file__).parent) as temporary:
        directory = Path(temporary)
        test = directory / "wifi_board_recovery_host.cc"
        test.write_text(body, encoding="utf-8")
        exe = directory / "wifi_board_recovery_host.exe"
        result = subprocess.run([str(compiler_bin / "cl.exe"), "/nologo", "/std:c++17", "/EHsc", "/utf-8",
                                 "/D_CRT_SECURE_NO_WARNINGS", str(test), "/Fe:" + str(exe),
                                 "/Fo:" + str(directory) + os.sep], env=env, cwd=directory,
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        subprocess.run([str(exe)], cwd=directory, check=True)


if __name__ == "__main__":
    main()
