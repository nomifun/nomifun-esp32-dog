"""Compile actual WifiStation and credential helpers with offline ESP/RTOS fakes.

Uses the installed Windows MSVC toolchain. No device, network, WSL or downloads.
"""

import os
from pathlib import Path
import subprocess
import tempfile

import test_saved_add as host


PLATFORM = r'''
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int BIT0 = 1, BIT1 = 2, BIT2 = 4;
constexpr int pdTRUE = 1, pdFALSE = 0, portTICK_PERIOD_MS = 1;
using EventGroupHandle_t = unsigned*;
EventGroupHandle_t xEventGroupCreate() { return new unsigned(0); }
void vEventGroupDelete(EventGroupHandle_t event) { delete event; }
unsigned xEventGroupSetBits(EventGroupHandle_t event, unsigned bits) { return *event |= bits; }
unsigned xEventGroupClearBits(EventGroupHandle_t event, unsigned bits) { return *event &= ~bits; }
unsigned xEventGroupGetBits(EventGroupHandle_t event) { return *event; }
unsigned xEventGroupWaitBits(EventGroupHandle_t event, unsigned bits, int, int, int) { return *event & bits; }
using esp_timer_handle_t = void*;
constexpr int ESP_TIMER_TASK = 0;
struct esp_timer_create_args_t {
    void (*callback)(void*); void* arg; int dispatch_method; const char* name; bool skip_unhandled_events;
};
int scheduled_scans = 0;
bool timer_created = false;
int esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t* timer) { timer_created = true; *timer = (void*)1; return 0; }
int esp_timer_start_once(esp_timer_handle_t, int) { ++scheduled_scans; return 0; }
int esp_timer_stop(esp_timer_handle_t) { return 0; }
int esp_timer_delete(esp_timer_handle_t) { return 0; }
using esp_event_base_t = const char*;
using esp_event_handler_instance_t = void*;
constexpr auto WIFI_EVENT = "WIFI_EVENT";
constexpr auto IP_EVENT = "IP_EVENT";
constexpr int ESP_EVENT_ANY_ID = -1;
constexpr int WIFI_EVENT_STA_START = 1, WIFI_EVENT_SCAN_DONE = 2;
constexpr int WIFI_EVENT_STA_DISCONNECTED = 3, WIFI_EVENT_STA_CONNECTED = 4, IP_EVENT_STA_GOT_IP = 5;
void (*wifi_handler)(void*, esp_event_base_t, int32_t, void*) = nullptr;
void* wifi_context = nullptr;
int esp_event_handler_instance_register(esp_event_base_t base, int, void (*handler)(void*, esp_event_base_t, int32_t, void*),
                                        void* context, esp_event_handler_instance_t* instance) {
    if (base == WIFI_EVENT) { wifi_handler = handler; wifi_context = context; }
    *instance = (void*)1; return 0;
}
int esp_event_handler_instance_unregister(esp_event_base_t, int, esp_event_handler_instance_t) { return 0; }
struct esp_netif_t {};
esp_netif_t* esp_netif_create_default_wifi_sta() { static esp_netif_t netif; return &netif; }
void esp_netif_destroy_default_wifi(esp_netif_t*) {}
struct ip_event_got_ip_t { struct { unsigned ip; } ip_info; };
void esp_ip4addr_ntoa(const unsigned*, char* output, size_t size) { std::strncpy(output, "192.168.1.10", size); }
struct wifi_event_sta_disconnected_t { int reason; };
using wifi_auth_mode_t = int;
struct wifi_ap_record_t {
    uint8_t bssid[6]; uint8_t ssid[33]; int8_t rssi; uint8_t primary; wifi_auth_mode_t authmode;
};
struct wifi_config_t {
    struct { uint8_t ssid[32]; uint8_t password[64]; uint8_t channel; uint8_t bssid[6]; bool bssid_set; int listen_interval; int scan_method; } sta;
};
constexpr int WIFI_MODE_STA = 1, WIFI_IF_STA = 0, WIFI_AUTH_OPEN = 0, WIFI_ALL_CHANNEL_SCAN = 1;
struct wifi_country_t { char cc[3]; uint8_t schan; uint8_t nchan; int policy; };
using wifi_ps_type_t = int;
constexpr int WIFI_PS_MAX_MODEM = 2, WIFI_PS_MIN_MODEM = 1, WIFI_PS_NONE = 0;
std::vector<wifi_config_t> configurations;
std::vector<wifi_ap_record_t> scanned_aps;
std::deque<int> config_results, connect_results, scan_results, scan_count_results, scan_record_results;
int connect_calls = 0;
int scan_calls = 0, scan_record_calls = 0, clear_ap_list_calls = 0;
bool synchronous_wifi_start = false, timer_ready_at_start = false;
int next_result(std::deque<int>& results) {
    if (results.empty()) return ESP_OK;
    const int result = results.front(); results.pop_front(); return result;
}
int esp_wifi_set_config(int, const wifi_config_t* config) {
    configurations.push_back(*config); return next_result(config_results);
}
int esp_wifi_connect() { ++connect_calls; return next_result(connect_results); }
int esp_wifi_disconnect() { return 0; }
int esp_wifi_scan_stop() { return 0; }
int esp_wifi_stop() { return 0; }
int esp_wifi_set_mode(int) { return 0; }
int esp_wifi_start() {
    timer_ready_at_start = timer_created;
    if (synchronous_wifi_start && wifi_handler) wifi_handler(wifi_context, WIFI_EVENT, WIFI_EVENT_STA_START, nullptr);
    return 0;
}
int esp_wifi_set_max_tx_power(int) { return 0; }
int esp_wifi_set_ps(int) { return 0; }
int esp_wifi_get_country(wifi_country_t* country) { *country = {{'0', '1', 0}, 1, 11, 0}; return 0; }
int esp_wifi_scan_start(void*, bool) { ++scan_calls; return next_result(scan_results); }
int esp_wifi_clear_ap_list() { ++clear_ap_list_calls; return 0; }
int esp_wifi_scan_get_ap_num(uint16_t* count) {
    *count = static_cast<uint16_t>(scanned_aps.size()); return next_result(scan_count_results);
}
int esp_wifi_scan_get_ap_records(uint16_t* count, wifi_ap_record_t* records) {
    ++scan_record_calls;
    if (const auto error = next_result(scan_record_results); error != ESP_OK) return error;
    for (unsigned i = 0; i < *count; ++i) records[i] = scanned_aps[i]; return 0;
}
int esp_wifi_sta_get_ap_info(wifi_ap_record_t* info) { *info = {}; return 0; }
using nvs_handle_t = int;
constexpr int NVS_READONLY = 0;
int nvs_open(const char*, int, nvs_handle_t*) { return ESP_FAIL; }
int nvs_get_i8(nvs_handle_t, const char*, int8_t*) { return ESP_FAIL; }
int nvs_get_u8(nvs_handle_t, const char*, uint8_t*) { return ESP_FAIL; }
void nvs_close(nvs_handle_t) {}
const char* esp_err_to_name(int) { return "injected driver error"; }
#define ESP_ERROR_CHECK(expression) do { if ((expression) != ESP_OK) throw std::runtime_error("Unexpected ESP_ERROR_CHECK"); } while (0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define bzero(pointer, length) std::memset(pointer, 0, length)
'''

CASES = r'''
#include "host_platform.h"
#define private public
#include "wifi_station.h"
#include "ssid_manager.h"
#undef private
#include "wifi_credentials.h"
#include "../wifi_station.cc"

SsidManager::SsidManager() = default;
SsidManager::~SsidManager() = default;
bool SsidManager::AddSsid(const std::string& ssid, const std::string& password) {
    ssid_list_.push_back({ssid, password}); return true;
}
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void reset() {
    configurations.clear(); scanned_aps.clear(); config_results.clear(); connect_results.clear();
    scan_results.clear(); scan_count_results.clear(); scan_record_results.clear();
    connect_calls = 0; scheduled_scans = 0;
    scan_calls = 0; scan_record_calls = 0; clear_ap_list_calls = 0;
    synchronous_wifi_start = false; timer_created = false; timer_ready_at_start = false;
    wifi_handler = nullptr; wifi_context = nullptr;
    SsidManager::GetInstance().ssid_list_.clear();
}
WifiApRecord record(std::string ssid = "Home", std::string password = "12345678") {
    return {ssid, password, 11, 3, {1, 2, 3, 4, 5, 6}};
}
int main() {
    int cases = 0;
    const std::vector<std::pair<std::string, std::string>> valid = {
        {"Home", ""}, {"Home", "12345678"}, {std::string(31, 's'), std::string(63, 'p')},
        {std::string(32, 's'), std::string(64, 'a')}, {std::string(10, 'x') + "家庭网络", "secret"}
    };
    for (const auto& credentials : valid) {
        struct Guarded { uint8_t before = 0xa5; uint8_t ssid[32]; uint8_t middle = 0xc3; uint8_t password[64]; uint8_t after = 0x5a; } output;
        std::memset(output.ssid, 0xee, sizeof(output.ssid)); std::memset(output.password, 0xee, sizeof(output.password));
        check(CopyWifiCredentials(credentials.first, credentials.second, output.ssid, output.password), "Valid credentials rejected");
        check(output.before == 0xa5 && output.middle == 0xc3 && output.after == 0x5a, "Credential copy overwrote array boundary");
        check(std::memcmp(output.ssid, credentials.first.data(), credentials.first.size()) == 0, "SSID truncated");
        check(std::memcmp(output.password, credentials.second.data(), credentials.second.size()) == 0, "Password truncated");
        for (size_t i = credentials.first.size(); i < sizeof(output.ssid); ++i) check(output.ssid[i] == 0, "SSID padding not cleared");
        for (size_t i = credentials.second.size(); i < sizeof(output.password); ++i) check(output.password[i] == 0, "Password padding not cleared");
        check(WifiSsidFromBytes(output.ssid) == credentials.first, "SSID byte decode changed length"); ++cases;
    }
    const std::vector<std::pair<std::string, std::string>> invalid = {
        {"", ""}, {std::string(33, 's'), ""}, {"Home", std::string(65, 'p')},
        {std::string("Bad\0hidden", 10), ""}, {"Home", std::string("pw\0hidden", 9)},
        {std::string(11, 'x') + "家庭网络家庭网络", ""}
    };
    for (const auto& credentials : invalid) {
        uint8_t ssid[32], password[64]; std::memset(ssid, 0xa5, sizeof(ssid)); std::memset(password, 0xc3, sizeof(password));
        check(!CopyWifiCredentials(credentials.first, credentials.second, ssid, password), "Invalid credentials accepted");
        check(std::all_of(std::begin(ssid), std::end(ssid), [](uint8_t value) { return value == 0xa5; }), "Invalid copy modified SSID");
        check(std::all_of(std::begin(password), std::end(password), [](uint8_t value) { return value == 0xc3; }), "Invalid copy modified password");
        reset(); WifiStation station; station.connect_queue_.push_back(record(credentials.first, credentials.second));
        station.StartConnect();
        check(configurations.empty() && connect_calls == 0 && scheduled_scans == 1, "Invalid saved credentials reached driver or did not recover"); ++cases;
    }
    {
        reset(); WifiStation station; station.StartConnect();
        check(configurations.empty() && scheduled_scans == 1, "Empty queue did not safely schedule scan"); ++cases;
    }
    {
        reset(); WifiStation station; station.remember_bssid_ = 1;
        station.connect_queue_ = {record("", "bad"), record(std::string(32, 's'), std::string(64, 'a'))};
        station.StartConnect();
        check(configurations.size() == 1 && connect_calls == 1 && scheduled_scans == 0, "Invalid candidate prevented next valid connection");
        const auto& config = configurations.front().sta;
        check(std::memcmp(config.ssid, std::string(32, 's').data(), 32) == 0, "Station truncated full SSID");
        check(std::memcmp(config.password, std::string(64, 'a').data(), 64) == 0, "Station truncated full PSK");
        check(config.channel == 11 && config.bssid_set && config.bssid[5] == 6 && config.listen_interval == 10, "Credential copy corrupted adjacent config"); ++cases;
    }
    {
        reset(); WifiStation station; station.connect_queue_ = {record("First"), record("Second")}; config_results = {ESP_FAIL, ESP_OK};
        station.StartConnect();
        check(configurations.size() == 2 && connect_calls == 1 && station.GetSsid() == "Second", "Rejected driver config aborted or blocked next candidate"); ++cases;
    }
    {
        reset(); WifiStation station; station.connect_queue_ = {record("First"), record("Second")}; connect_results = {ESP_FAIL, ESP_OK};
        station.StartConnect();
        check(connect_calls == 2 && station.GetSsid() == "Second" && scheduled_scans == 0, "Driver connect failure aborted or blocked next candidate"); ++cases;
    }
    {
        reset(); WifiStation station; station.connect_queue_ = {record()}; connect_results = {ESP_FAIL};
        station.StartConnect(); check(scheduled_scans == 1, "All driver failures did not return to scan"); ++cases;
    }
    {
        reset(); WifiStation station; station.last_disconnect_reason_ = 202; station.ip_address_ = "stale";
        station.was_connected_ = true; station.connect_queue_.push_back(record()); station.Start();
        check(station.GetLastDisconnectReason() == 0 && station.GetIpAddress().empty() && !station.was_connected_ && station.connect_queue_.empty(), "Start retained stale session failure state"); ++cases;
        wifi_event_sta_disconnected_t disconnected{202};
        WifiStation::WifiEventHandler(&station, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
        check(station.GetLastDisconnectReason() == 202, "Disconnect reason was not exposed"); ++cases;
        station.Stop(); check(station.GetLastDisconnectReason() == 202, "Stop erased failure needed by recovery UI"); ++cases;
    }
    {
        reset(); WifiStation station; station.last_disconnect_reason_ = 15;
        ip_event_got_ip_t ip{}; WifiStation::IpEventHandler(&station, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
        check(station.GetLastDisconnectReason() == 0 && station.GetIpAddress() == "192.168.1.10" && station.IsConnected(), "IP success did not clear old failure"); ++cases;
    }
    {
        reset(); WifiStation station; const std::string full_ssid(32, 's');
        SsidManager::GetInstance().AddSsid(full_ssid, std::string(64, 'a'));
        wifi_ap_record_t ap{}; std::memcpy(ap.ssid, full_ssid.data(), 32); ap.primary = 6; scanned_aps.push_back(ap);
        station.HandleScanResult();
        check(connect_calls == 1 && station.GetSsid() == full_ssid, "Full-width scan SSID did not match saved name"); ++cases;
    }
    {
        reset(); WifiStation station; station.remember_bssid_ = 1;
        SsidManager::GetInstance().AddSsid("Hidden", "hidden-password");
        station.HandleScanResult();
        check(connect_calls == 1 && station.GetSsid() == "Hidden", "Saved SSID absent from scan was never attempted");
        const auto& config = configurations.front().sta;
        check(config.scan_method == WIFI_ALL_CHANNEL_SCAN && config.channel == 0 && !config.bssid_set,
              "Direct attempt must probe all channels without stale BSSID lock");
        check(std::memcmp(config.password, "hidden-password", 15) == 0, "Direct attempt changed saved password");
        check(scan_record_calls == 0, "Zero-AP scan passed null records to driver"); ++cases;
        wifi_event_sta_disconnected_t disconnected{202};
        for (int i = 0; i < 6; ++i) WifiStation::WifiEventHandler(&station, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
        check(connect_calls == 6 && scheduled_scans == 1, "Direct attempt did not use bounded reconnect count"); ++cases;
        station.HandleScanResult(); station.HandleScanResult();
        check(connect_calls == 6 && scheduled_scans == 3 && station.GetLastDisconnectReason() == 202,
              "No-match scans restarted direct attempt indefinitely or erased real failure"); ++cases;
        station.Stop(); check(station.GetLastDisconnectReason() == 202, "Direct failure reason lost before provisioning fallback"); ++cases;
    }
    {
        reset(); WifiStation station;
        SsidManager::GetInstance().AddSsid("Saved", "12345678");
        wifi_ap_record_t unrelated{}; std::memcpy(unrelated.ssid, "Other", 5); scanned_aps.push_back(unrelated);
        station.HandleScanResult();
        check(connect_calls == 1 && station.GetSsid() == "Saved" && scan_record_calls == 1,
              "Nonempty scan with no matching saved entry suppressed direct attempt"); ++cases;
    }
    {
        reset(); WifiStation station;
        SsidManager::GetInstance().AddSsid("", "bad");
        SsidManager::GetInstance().AddSsid("First", "11111111");
        SsidManager::GetInstance().AddSsid("Second", "22222222");
        config_results = {ESP_FAIL, ESP_OK};
        station.HandleScanResult();
        check(configurations.size() == 2 && connect_calls == 1 && station.GetSsid() == "Second",
              "Invalid saved entry or driver rejection blocked next direct candidate"); ++cases;
    }
    {
        reset(); WifiStation station; station.HandleScanResult();
        check(connect_calls == 0 && configurations.empty() && scan_record_calls == 0 && scheduled_scans == 1,
              "No saved entries and zero APs did not safely schedule retry"); ++cases;
    }
    {
        reset(); WifiStation station; station.remember_bssid_ = 1;
        SsidManager::GetInstance().AddSsid("Visible", "12345678");
        wifi_ap_record_t ap{}; std::memcpy(ap.ssid, "Visible", 7); ap.primary = 6; ap.bssid[5] = 77; scanned_aps.push_back(ap);
        station.HandleScanResult();
        check(connect_calls == 1 && !station.direct_connect_attempted_ && configurations.front().sta.bssid_set &&
              configurations.front().sta.channel == 6 && configurations.front().sta.bssid[5] == 77,
              "Scan match lost priority or its explicit BSSID/channel policy"); ++cases;
    }
    {
        reset(); WifiStation station; station.direct_connect_attempted_ = true; station.ssid_ = "stale";
        station.scan_current_interval_microseconds_ = 123; station.Start();
        check(!station.direct_connect_attempted_ && station.GetSsid().empty() &&
              station.scan_current_interval_microseconds_ == station.scan_min_interval_microseconds_,
              "New station session retained direct-attempt or retry state"); ++cases;
        station.direct_connect_attempted_ = true;
        ip_event_got_ip_t ip{}; WifiStation::IpEventHandler(&station, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
        check(!station.direct_connect_attempted_, "Successful IP did not rearm later recovery session"); ++cases;
    }
    {
        reset(); WifiStation station; synchronous_wifi_start = true; scan_results = {ESP_FAIL, ESP_OK};
        station.Start();
        check(timer_ready_at_start && scan_calls == 1 && scheduled_scans == 1,
              "Immediate STA_START/scan failure ran without initialized retry timer"); ++cases;
        station.StartScan();
        check(scan_calls == 2 && scheduled_scans == 1, "Scan retry success scheduled a duplicate retry"); ++cases;
    }
    {
        reset(); WifiStation station; SsidManager::GetInstance().AddSsid("Saved", "12345678");
        scan_count_results = {ESP_FAIL}; station.HandleScanResult();
        check(connect_calls == 0 && scheduled_scans == 1 && clear_ap_list_calls == 1 && !station.direct_connect_attempted_,
              "Failed scan-count read masqueraded as empty scan or leaked driver records"); ++cases;
        station.HandleScanResult();
        check(connect_calls == 1, "Later valid scan could not use direct fallback after count read failure"); ++cases;
    }
    {
        reset(); WifiStation station; SsidManager::GetInstance().AddSsid("Saved", "12345678");
        wifi_ap_record_t ap{}; scanned_aps.push_back(ap); scan_record_results = {ESP_FAIL}; station.HandleScanResult();
        check(connect_calls == 0 && scheduled_scans == 1 && clear_ap_list_calls == 1 && !station.direct_connect_attempted_,
              "Failed scan-record read reached connector or leaked driver records"); ++cases;
    }
    {
        reset(); WifiStation station; station.connect_queue_.push_back(record("Next")); connect_results = {ESP_FAIL, ESP_OK};
        wifi_event_sta_disconnected_t disconnected{201};
        WifiStation::WifiEventHandler(&station, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnected);
        check(connect_calls == 2 && station.GetSsid() == "Next" && station.GetLastDisconnectReason() == 201,
              "Synchronous reconnect error stalled candidate queue or erased disconnect reason"); ++cases;
    }
    std::cout << cases << " actual WifiStation/credential behavior cases passed\n";
}
'''


def main():
    env = os.environ.copy()
    compiler_bin = host.VC / "bin" / "Hostx64" / "x64"
    env["PATH"] = str(compiler_bin) + os.pathsep + env["PATH"]
    env["INCLUDE"] = os.pathsep.join(str(path) for path in
        (host.VC / "include", host.SDK / "Include" / host.SDK_VERSION / "ucrt",
         host.SDK / "Include" / host.SDK_VERSION / "shared", host.SDK / "Include" / host.SDK_VERSION / "um"))
    env["LIB"] = os.pathsep.join(str(path) for path in
        (host.VC / "lib" / "x64", host.SDK / "Lib" / host.SDK_VERSION / "ucrt" / "x64",
         host.SDK / "Lib" / host.SDK_VERSION / "um" / "x64"))
    with tempfile.TemporaryDirectory(prefix="station-test-", dir=Path(__file__).parent) as temporary:
        directory = Path(temporary)
        (directory / "host_platform.h").write_text(PLATFORM, encoding="utf-8")
        for name in ("esp_event.h", "esp_timer.h", "esp_netif.h", "esp_wifi_types_generic.h",
                     "esp_log.h", "esp_wifi.h", "nvs.h", "nvs_flash.h", "esp_system.h",
                     "freertos/FreeRTOS.h", "freertos/event_groups.h"):
            header = directory / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "host_platform.h"\n', encoding="utf-8")
        test = directory / "wifi_station_host.cc"
        source_path = (host.COMPONENT / "wifi_station.cc").as_posix()
        test.write_text(CASES.replace('#include "../wifi_station.cc"', '#include "' + source_path + '"'), encoding="utf-8")
        exe = directory / "wifi_station_host.exe"
        result = subprocess.run([str(compiler_bin / "cl.exe"), "/nologo", "/std:c++20", "/EHsc", "/utf-8",
                                 "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(directory),
                                 "/I" + str(host.COMPONENT / "include"), str(test),
                                 "/Fe:" + str(exe), "/Fo:" + str(directory) + os.sep],
                                env=env, cwd=directory, capture_output=True, text=True,
                                encoding="utf-8", errors="replace")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        subprocess.run([str(exe)], cwd=directory, check=True)


if __name__ == "__main__":
    main()
