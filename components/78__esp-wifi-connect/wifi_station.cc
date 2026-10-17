#include "wifi_station.h"
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include "ssid_manager.h"
#include "wifi_credentials.h"

#define TAG "WifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_STOPPED BIT1
#define WIFI_EVENT_SCAN_DONE_BIT BIT2
#define MAX_RECONNECT_COUNT 5

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        max_tx_power_ = 0;
        remember_bssid_ = 0;
    } else {
        err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
        if (err != ESP_OK) {
            max_tx_power_ = 0;
        }
        err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
        if (err != ESP_OK) {
            remember_bssid_ = 0;
        }
        nvs_close(nvs);
    }
}

WifiStation::~WifiStation() {
    Stop();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    ESP_LOGI(TAG, "Stopping WiFi station");
    
    // Unregister event handlers FIRST to prevent scan done from triggering connect
    if (instance_any_id_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }

    // Stop timer
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }

    // Now safe to stop scan, disconnect and stop WiFi (no event callbacks will fire)
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (station_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(station_netif_);
        station_netif_ = nullptr;
    }
    
    // Reset was_connected_ flag to prevent stale state from affecting subsequent sessions
    was_connected_ = false;

    // Clear connected bit
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    
    // Set stopped event AFTER cleanup is complete to unblock WaitForConnected
    // This ensures no race condition with subsequent WiFi operations
    xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED);
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::OnDisconnected(std::function<void(int reason)> on_disconnected) {
    on_disconnected_ = on_disconnected;
}

void WifiStation::Start() {
    // Note: esp_netif_init() and esp_wifi_init() should be called once before calling this method
    // WiFi driver is initialized by WifiManager::Initialize() and kept alive
    
    last_disconnect_reason_.store(0);
    connect_queue_.clear();
    was_connected_ = false;
    ip_address_.clear();
    ssid_.clear();
    password_.clear();
    direct_connect_attempted_ = false;
    scan_current_interval_microseconds_ = scan_min_interval_microseconds_;

    // Clear stopped event bit so WaitForConnected works properly
    // Clear scan done bit so Stop() can wait for scan to complete
    xEventGroupClearBits(event_group_, WIFI_EVENT_STOPPED | WIFI_EVENT_SCAN_DONE_BIT);
    
    // Create the default WiFi station interface
    station_netif_ = esp_netif_create_default_wifi_sta();

    // Create the retry timer before starting WiFi: a fast STA_START/SCAN_DONE
    // event (including an immediate scan failure) must already have a timer.
    // skip_unhandled_events = false so the timer can wake the CPU from light
    // sleep on its own; otherwise an idle device that failed to connect would
    // never retry the scan, because esp_timer_get_next_alarm_for_wake_up
    // (components/esp_timer/src/esp_timer.c) excludes timers with this flag
    // from light-sleep wakeup sources.
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            static_cast<WifiStation*>(arg)->StartScan();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = false
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_country_t country = {};
    const auto country_error = esp_wifi_get_country(&country);
    if (country_error == ESP_OK) {
        ESP_LOGI(TAG, "WiFi scan country=%.2s channels=%u..%u policy=%d", country.cc,
            static_cast<unsigned>(country.schan), static_cast<unsigned>(country.schan + country.nchan - 1),
            static_cast<int>(country.policy));
    } else {
        ESP_LOGW(TAG, "Could not read WiFi country: %s", esp_err_to_name(country_error));
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    // Wait for either connected or stopped event
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED, 
                                    pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    // Return true only if connected (not if stopped)
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::ScheduleNextScan() {
    const auto error = esp_timer_start_once(timer_handle_, scan_current_interval_microseconds_);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not schedule WiFi scan: %s", esp_err_to_name(error));
        return;
    }
    UpdateScanInterval();
}

void WifiStation::StartScan() {
    const auto error = esp_wifi_scan_start(nullptr, false);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not start WiFi scan: %s", esp_err_to_name(error));
        ScheduleNextScan();
    }
}

void WifiStation::HandleScanResult() {
    uint16_t ap_num = 0;
    auto error = esp_wifi_scan_get_ap_num(&ap_num);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not read WiFi scan count: %s", esp_err_to_name(error));
        esp_wifi_clear_ap_list();
        ScheduleNextScan();
        return;
    }
    std::unique_ptr<wifi_ap_record_t, decltype(&std::free)> ap_records(nullptr, &std::free);
    if (ap_num != 0) {
        ap_records.reset(static_cast<wifi_ap_record_t*>(std::malloc(ap_num * sizeof(wifi_ap_record_t))));
        if (!ap_records) {
            ESP_LOGW(TAG, "Not enough memory for %u WiFi scan records", ap_num);
            esp_wifi_clear_ap_list();
            ScheduleNextScan();
            return;
        }
        error = esp_wifi_scan_get_ap_records(&ap_num, ap_records.get());
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Could not read WiFi scan records: %s", esp_err_to_name(error));
            esp_wifi_clear_ap_list();
            ScheduleNextScan();
            return;
        }
        // Sort only allocated, populated records; zero APs have no array.
        if (ap_num > 1) {
            std::sort(ap_records.get(), ap_records.get() + ap_num, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
                return a.rssi > b.rssi;
            });
        }
    }

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    for (int i = 0; i < ap_num; i++) {
        auto ap_record = ap_records.get()[i];
        const auto scanned_ssid = WifiSsidFromBytes(ap_record.ssid);
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [&scanned_ssid](const SsidItem& item) {
            return scanned_ssid == item.ssid;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                scanned_ssid.c_str(),
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = it->ssid,
                .password = it->password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode,
                .bssid = {0}
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            connect_queue_.push_back(record);
        }
    }
    ESP_LOGI(TAG, "WiFi scan: total=%u saved=%u matched=%u", ap_num,
        static_cast<unsigned>(ssid_list.size()), static_cast<unsigned>(connect_queue_.size()));

    if (connect_queue_.empty() && !direct_connect_attempted_) {
        // A visible broadcast scan is not a connection prerequisite. Hidden
        // networks and temporarily missing scan entries still need the driver
        // to probe their exact saved SSID. Do this once, without BSSID/channel
        // locking, rather than silently scanning until the board's timeout.
        direct_connect_attempted_ = true;
        for (const auto& item : ssid_list) {
            if (!IsWifiCredentialsValid(item.ssid, item.password)) continue;
            connect_queue_.push_back({item.ssid, item.password, 0, WIFI_AUTH_OPEN, {0}, false});
        }
        ESP_LOGI(TAG, "WiFi direct fallback: candidates=%u", static_cast<unsigned>(connect_queue_.size()));
    }

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "No AP found, next scan in %d seconds", scan_current_interval_microseconds_ / 1000 / 1000);
        ScheduleNextScan();
        return;
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    while (!connect_queue_.empty()) {
        auto ap_record = std::move(connect_queue_.front());
        connect_queue_.erase(connect_queue_.begin());

        wifi_config_t wifi_config = {};
        if (!CopyWifiCredentials(ap_record.ssid, ap_record.password,
                                 wifi_config.sta.ssid, wifi_config.sta.password)) {
            ESP_LOGW(TAG, "Skipping invalid saved WiFi credentials");
            continue;
        }
        if (remember_bssid_ && ap_record.from_scan) {
            wifi_config.sta.channel = ap_record.channel;
            memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
            wifi_config.sta.bssid_set = true;
        }
        if (!ap_record.from_scan) wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config.sta.listen_interval = 10;
        auto error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Skipping WiFi configuration rejected by driver: %s", esp_err_to_name(error));
            continue;
        }

        ssid_ = ap_record.ssid;
        password_ = ap_record.password;
        if (on_connect_) on_connect_(ssid_);
        reconnect_count_ = 0;
        ESP_LOGI(TAG, "WiFi connection source=%s ssid_bytes=%u channel=%u bssid_lock=%d",
            ap_record.from_scan ? "scanned" : "direct", static_cast<unsigned>(ssid_.size()),
            wifi_config.sta.channel, wifi_config.sta.bssid_set);
        error = esp_wifi_connect();
        if (error == ESP_OK) return;
        ESP_LOGW(TAG, "WiFi connect could not start: %s", esp_err_to_name(error));
    }

    // An empty or entirely invalid saved list must not dereference front() or
    // abort the firmware. Keep the normal bounded startup timeout/recovery path.
    ScheduleNextScan();
}

int8_t WifiStation::GetRssi() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }
    
    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }
    
    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetScanIntervalRange(int min_interval_seconds, int max_interval_seconds) {
    scan_min_interval_microseconds_ = min_interval_seconds * 1000 * 1000;
    scan_max_interval_microseconds_ = max_interval_seconds * 1000 * 1000;
    scan_current_interval_microseconds_ = scan_min_interval_microseconds_;
}

void WifiStation::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    wifi_ps_type_t ps_type;
    switch (level) {
        case WifiPowerSaveLevel::LOW_POWER:
            ps_type = WIFI_PS_MAX_MODEM;  // Maximum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: LOW_POWER (MAX_MODEM)");
            break;
        case WifiPowerSaveLevel::BALANCED:
            ps_type = WIFI_PS_MIN_MODEM;  // Minimum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: BALANCED (MIN_MODEM)");
            break;
        case WifiPowerSaveLevel::PERFORMANCE:
        default:
            ps_type = WIFI_PS_NONE;       // No power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: PERFORMANCE (NONE)");
            break;
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(ps_type));
}

void WifiStation::UpdateScanInterval() {
    // Apply exponential backoff: double the interval, up to max
    if (scan_current_interval_microseconds_ < scan_max_interval_microseconds_) {
        scan_current_interval_microseconds_ *= 2;
        if (scan_current_interval_microseconds_ > scan_max_interval_microseconds_) {
            scan_current_interval_microseconds_ = scan_max_interval_microseconds_;
        }
    }
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (event_id == WIFI_EVENT_STA_START) {
        this_->StartScan();
        if (this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(this_->event_group_, WIFI_EVENT_SCAN_DONE_BIT);
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        
        // Notify disconnected callback only once when transitioning from connected to disconnected
        bool was_connected = this_->was_connected_;
        this_->was_connected_ = false;
        wifi_event_sta_disconnected_t* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        this_->last_disconnect_reason_.store(event->reason);
        ESP_LOGI(TAG, "WiFi disconnected, reason: %d", event->reason);
        if (was_connected && this_->on_disconnected_) {
            this_->on_disconnected_(event->reason);
        }
        
        if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
            const auto error = esp_wifi_connect();
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting WiFi (attempt %d / %d)", this_->reconnect_count_, MAX_RECONNECT_COUNT);
            if (error == ESP_OK) return;
            ESP_LOGW(TAG, "WiFi reconnect could not start: %s", esp_err_to_name(error));
        }

        if (!this_->connect_queue_.empty()) {
            this_->StartConnect();
            return;
        }
        
        ESP_LOGI(TAG, "No more AP to connect, next scan in %d seconds", 
                 this_->scan_current_interval_microseconds_ / 1000 / 1000);
        this_->ScheduleNextScan();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    this_->last_disconnect_reason_.store(0);
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());
    
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    this_->was_connected_ = true;  // Mark as connected for disconnect notification
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
    this_->direct_connect_attempted_ = false;
    
    // Reset scan interval to minimum for fast reconnect if disconnected later
    this_->scan_current_interval_microseconds_ = this_->scan_min_interval_microseconds_;
}
