"""Run real AP lifecycle/compatibility methods with offline ESP/RTOS fakes.

No Wi-Fi adapter, network, serial port or device is accessed.
"""

import os
from pathlib import Path
import subprocess
import tempfile

import test_saved_add as host


PLATFORM = r'''
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "wifi_credentials.h"
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1;
constexpr unsigned BIT0 = 1, BIT1 = 2, WIFI_CONNECTED_BIT = BIT0, WIFI_FAIL_BIT = BIT1;
constexpr int pdTRUE = 1, pdFALSE = 0;
using EventBits_t = unsigned;
using EventGroupHandle_t = unsigned*;
unsigned xEventGroupSetBits(EventGroupHandle_t event, unsigned bits) { return *event |= bits; }
unsigned xEventGroupClearBits(EventGroupHandle_t event, unsigned bits) { return *event &= ~bits; }
unsigned xEventGroupWaitBits(EventGroupHandle_t event, unsigned mask, int clear, int, unsigned) {
    const auto bits = *event & mask; if (clear) *event &= ~mask; return bits;
}
unsigned pdMS_TO_TICKS(unsigned milliseconds) { return milliseconds; }
void vTaskDelay(unsigned) {}
using esp_event_base_t = const char*;
using esp_event_handler_instance_t = void*;
constexpr auto WIFI_EVENT = "wifi", IP_EVENT = "ip";
constexpr int ESP_EVENT_ANY_ID = -1, WIFI_EVENT_STA_CONNECTED = 1, WIFI_EVENT_STA_DISCONNECTED = 2;
constexpr int WIFI_EVENT_SCAN_DONE = 3, WIFI_EVENT_AP_STACONNECTED = 4, WIFI_EVENT_AP_STADISCONNECTED = 5;
constexpr int IP_EVENT_STA_GOT_IP = 6;
int esp_event_handler_instance_unregister(esp_event_base_t, int, void*) { return ESP_OK; }
using esp_timer_handle_t = void*;
int esp_timer_start_once(void*, unsigned) { return ESP_OK; }
int esp_timer_stop(void*) { return ESP_OK; }
int esp_timer_delete(void*) { return ESP_OK; }
struct wifi_event_ap_staconnected_t { uint8_t mac[6]; int aid; };
using wifi_event_ap_stadisconnected_t = wifi_event_ap_staconnected_t;
struct wifi_ap_record_t { uint8_t ssid[33]; };
struct wifi_config_t {
    struct { uint8_t ssid[32]; uint8_t password[64]; int scan_method; int failure_retry_cnt; } sta;
    struct { uint8_t ssid[33]; size_t ssid_len; int max_connection; int authmode; } ap;
};
struct esp_netif_t { bool station; };
std::set<esp_netif_t*> live_netifs;
int sta_created = 0, ap_created = 0, destroyed = 0;
bool wifi_stopped = false;
esp_netif_t* create_netif(bool station) {
    for (auto* netif : live_netifs) if (netif->station == station) throw std::runtime_error("Duplicate default netif");
    auto* netif = new esp_netif_t{station}; live_netifs.insert(netif);
    station ? ++sta_created : ++ap_created; return netif;
}
esp_netif_t* esp_netif_create_default_wifi_sta() { return create_netif(true); }
esp_netif_t* esp_netif_create_default_wifi_ap() { return create_netif(false); }
void esp_netif_destroy_default_wifi(esp_netif_t* netif) {
    if (!wifi_stopped || live_netifs.erase(netif) != 1) throw std::runtime_error("Netif destroyed before stop or twice");
    ++destroyed; delete netif;
}
struct esp_netif_ip_info_t { unsigned ip, gw, netmask; };
struct ip_event_got_ip_t { esp_netif_ip_info_t ip_info; };
#define IP4_ADDR(pointer, a, b, c, d) (*(pointer) = (a << 24 | b << 16 | c << 8 | d))
int esp_netif_dhcps_stop(esp_netif_t*) { return ESP_OK; }
int esp_netif_set_ip_info(esp_netif_t*, esp_netif_ip_info_t*) { return ESP_OK; }
int esp_netif_dhcps_start(esp_netif_t*) { return ESP_OK; }
struct DnsServer { void Start(unsigned) {} void Stop() {} };
constexpr int WIFI_MODE_APSTA = 3, WIFI_IF_AP = 1, WIFI_IF_STA = 0;
constexpr int WIFI_AUTH_OPEN = 0, WIFI_PS_NONE = 0, WIFI_BAND_MODE_2G_ONLY = 0, WIFI_ALL_CHANNEL_SCAN = 1;
std::function<void()> driver_events;
std::vector<wifi_config_t> station_configs;
int connects = 0, disconnects = 0;
int esp_wifi_set_mode(int) { return ESP_OK; }
int esp_wifi_set_config(int interface_id, const wifi_config_t* config) {
    if (interface_id == WIFI_IF_STA) station_configs.push_back(*config); return ESP_OK;
}
int esp_wifi_set_ps(int) { return ESP_OK; }
int esp_wifi_start() { wifi_stopped = false; return ESP_OK; }
int esp_wifi_stop() { wifi_stopped = true; return ESP_OK; }
int esp_wifi_set_band_mode(int) { return ESP_OK; }
int esp_wifi_set_max_tx_power(int) { return ESP_OK; }
int esp_wifi_get_max_tx_power(int8_t*) { return ESP_OK; }
int esp_wifi_connect() { ++connects; if (driver_events) driver_events(); return ESP_OK; }
int esp_wifi_disconnect() { ++disconnects; return ESP_OK; }
int esp_wifi_scan_stop() { return ESP_OK; }
int esp_wifi_scan_get_ap_num(uint16_t* count) { *count = 0; return ESP_OK; }
int esp_wifi_scan_get_ap_records(uint16_t*, wifi_ap_record_t*) { return ESP_OK; }
int httpd_stop(void*) { return ESP_OK; }
using nvs_handle_t = int;
constexpr int NVS_READONLY = 0;
int nvs_open(const char*, int, nvs_handle_t*) { return ESP_FAIL; }
int nvs_get_str(int, const char*, char*, size_t*) { return ESP_FAIL; }
int nvs_get_i8(int, const char*, int8_t*) { return ESP_FAIL; }
int nvs_get_u8(int, const char*, uint8_t*) { return ESP_FAIL; }
void nvs_close(int) {}
#define CONFIG_IDF_TARGET_ESP32P4 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERROR_CHECK(expression) do { if ((expression) != ESP_OK) throw std::runtime_error("Driver failure"); } while (0)
#define bzero(pointer, length) std::memset(pointer, 0, length)
struct WifiConfigurationAp {
    unsigned bits = 0;
    EventGroupHandle_t event_group_ = &bits;
    bool is_connecting_ = false;
    std::mutex mutex_;
    std::vector<wifi_ap_record_t> ap_records_;
    esp_timer_handle_t scan_timer_ = nullptr;
    esp_event_handler_instance_t instance_any_id_ = nullptr, instance_got_ip_ = nullptr;
    void* server_ = nullptr;
    std::unique_ptr<DnsServer> dns_server_;
    esp_netif_t* ap_netif_ = nullptr;
    esp_netif_t* station_netif_ = nullptr;
    std::string ota_url_;
    int8_t max_tx_power_ = 0;
    bool remember_bssid_ = false, sleep_mode_ = false;
    std::string GetSsid() { return "Xiaozhi-test"; }
    void StartAccessPoint();
    void Stop();
    bool ConnectToWifi(const std::string&, const std::string&);
    static void WifiEventHandler(void*, esp_event_base_t, int32_t, void*);
    static void IpEventHandler(void*, esp_event_base_t, int32_t, void*);
};
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
'''

CASES = r'''
int main() {
    int cases = 0;
    WifiConfigurationAp ap;
    for (int cycle = 0; cycle < 3; ++cycle) {
        ap.StartAccessPoint();
        check(live_netifs.size() == 2 && ap.ap_netif_ && ap.station_netif_, "APSTA did not own both netifs"); ++cases;
        ap.Stop();
        check(live_netifs.empty() && !ap.ap_netif_ && !ap.station_netif_ && !ap.dns_server_, "AP stop leaked netif or DNS"); ++cases;
        ap.Stop(); check(live_netifs.empty(), "Repeated stop destroyed interface twice"); ++cases;
    }
    check(sta_created == 3 && ap_created == 3 && destroyed == 6, "Netif lifecycle counts differ"); ++cases;
    ap.StartAccessPoint();
    driver_events = [&] { WifiConfigurationAp::WifiEventHandler(&ap, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, nullptr); };
    check(!ap.ConnectToWifi("Home", "12345678"), "Association alone was accepted without DHCP"); ++cases;
    check(!ap.is_connecting_ && connects == 2, "No-DHCP attempts did not finish bounded retry"); ++cases;
    driver_events = [&] {
        WifiConfigurationAp::WifiEventHandler(&ap, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, nullptr);
        ip_event_got_ip_t ip{}; WifiConfigurationAp::IpEventHandler(&ap, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    };
    check(ap.ConnectToWifi(std::string(32, 's'), std::string(64, 'a')), "GOT_IP did not complete compatibility connection"); ++cases;
    const auto& config = station_configs.back().sta;
    check(std::memcmp(config.ssid, std::string(32, 's').data(), 32) == 0 &&
          std::memcmp(config.password, std::string(64, 'a').data(), 64) == 0, "AP compatibility credentials were truncated"); ++cases;
    check(!ap.is_connecting_ && disconnects >= 3, "Compatibility connection did not clean its trial link"); ++cases;
    driver_events = [&] { WifiConfigurationAp::WifiEventHandler(&ap, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, nullptr); };
    check(!ap.ConnectToWifi("Home", "badpassword"), "Disconnected compatibility attempt succeeded"); ++cases;
    ap.Stop(); check(live_netifs.empty(), "Final stop leaked interfaces"); ++cases;
    std::cout << cases << " APSTA lifecycle and real compatibility connection cases passed\n";
}
'''


def extract_method(source, name):
    start = source.index(name)
    start = source.rfind("\n", 0, start) + 1
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def main():
    source = (host.COMPONENT / "wifi_configuration_ap.cc").read_text(encoding="utf-8")
    methods = "\n".join(extract_method(source, "WifiConfigurationAp::" + name) for name in
                        ("StartAccessPoint()", "Stop()", "ConnectToWifi(", "WifiEventHandler(", "IpEventHandler("))
    env = os.environ.copy()
    compiler_bin = host.VC / "bin" / "Hostx64" / "x64"
    env["PATH"] = str(compiler_bin) + os.pathsep + env["PATH"]
    env["INCLUDE"] = os.pathsep.join(str(path) for path in
        (host.VC / "include", host.SDK / "Include" / host.SDK_VERSION / "ucrt",
         host.SDK / "Include" / host.SDK_VERSION / "shared", host.SDK / "Include" / host.SDK_VERSION / "um"))
    env["LIB"] = os.pathsep.join(str(path) for path in
        (host.VC / "lib" / "x64", host.SDK / "Lib" / host.SDK_VERSION / "ucrt" / "x64",
         host.SDK / "Lib" / host.SDK_VERSION / "um" / "x64"))
    with tempfile.TemporaryDirectory(prefix="ap-connection-", dir=Path(__file__).parent) as temporary:
        directory = Path(temporary)
        test = directory / "ap_connection_host.cc"
        test.write_text(PLATFORM + methods + CASES, encoding="utf-8")
        exe = directory / "ap_connection_host.exe"
        result = subprocess.run([str(compiler_bin / "cl.exe"), "/nologo", "/std:c++17", "/EHsc", "/utf-8",
                                 "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(host.COMPONENT / "include"),
                                 str(test), "/Fe:" + str(exe), "/Fo:" + str(directory) + os.sep],
                                env=env, cwd=directory, capture_output=True, text=True,
                                encoding="utf-8", errors="replace")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        subprocess.run([str(exe)], cwd=directory, check=True)


if __name__ == "__main__":
    main()
