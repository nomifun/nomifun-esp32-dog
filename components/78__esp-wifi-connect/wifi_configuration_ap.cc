#include "wifi_configuration_ap.h"
#include <cstdio>
#include <charconv>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <lwip/ip_addr.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <cJSON.h>
#if !CONFIG_IDF_TARGET_ESP32P4
#include <esp_smartconfig.h>
#endif
#include "ssid_manager.h"
#include "saved_wifi_request.h"
#include "wifi_credentials.h"
#include "provisioning_settings.h"
#include "sdkconfig.h"

#define TAG "WifiConfigurationAp"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

extern const char index_html_start[] asm("_binary_wifi_configuration_html_start");
extern const char done_html_start[] asm("_binary_wifi_configuration_done_html_start");

// Build the complete list before sending any bytes, so allocation failures can
// return an HTTP error instead of a truncated JSON response with status 200.
static esp_err_t SendWifiListJson(httpd_req_t* request, const cJSON* json) {
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    char* encoded = json ? cJSON_PrintUnformatted(json) : nullptr;
    if (!encoded) {
        httpd_resp_set_status(request, "500 Internal Server Error");
        return httpd_resp_send(request, "{\"success\":false,\"error\":\"Failed to encode Wi-Fi list\"}", HTTPD_RESP_USE_STRLEN);
    }
    const esp_err_t result = httpd_resp_send(request, encoded, HTTPD_RESP_USE_STRLEN);
    cJSON_free(encoded);
    return result;
}

static bool ReadProvisioningBody(httpd_req_t* request, std::string& body) {
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Connection", "close");
    if (request->content_len == 0 || request->content_len > 1024) {
        httpd_resp_set_status(request, "400 Bad Request");
        httpd_resp_send(request, "{\"success\":false,\"error\":\"Invalid request size\"}", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    body.resize(request->content_len);
    size_t received = 0;
    while (received < body.size()) {
        const int result = httpd_req_recv(request, body.data() + received, body.size() - received);
        if (result <= 0) {
            httpd_resp_set_status(request, result == HTTPD_SOCK_ERR_TIMEOUT ? "408 Request Timeout" : "400 Bad Request");
            httpd_resp_send(request, "{\"success\":false,\"error\":\"Incomplete request body\"}", HTTPD_RESP_USE_STRLEN);
            return false;
        }
        received += result;
    }
    return true;
}

static esp_err_t ProvisioningFailure(httpd_req_t* request, const char* status, const char* message) {
    httpd_resp_set_status(request, status);
    std::string response = std::string("{\"success\":false,\"error\":\"") + message + "\"}";
    return httpd_resp_send(request, response.c_str(), HTTPD_RESP_USE_STRLEN);
}

// Prepare the worker before committing credentials so an accepted request can
// always leave AP mode. It waits for the handler to finish sending its response;
// notification is still required when that response is lost on the phone.
static bool PrepareProvisioningExitTask(std::function<void()>* callback, TaskHandle_t* task) {
    if (!callback || !*callback) return false;
    return xTaskCreate([](void* context) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1000));
        (*static_cast<std::function<void()>*>(context))();
        vTaskDelete(nullptr);
    }, "exit_config_task", 4096, callback, 5, task) == pdPASS;
}

WifiConfigurationAp::WifiConfigurationAp()
{
    event_group_ = xEventGroupCreate();
    language_ = "zh-CN";
    sleep_mode_ = false;
    instance_any_id_ = nullptr;
    instance_got_ip_ = nullptr;
    max_tx_power_ = 0;
    remember_bssid_ = false;
}

std::vector<wifi_ap_record_t> WifiConfigurationAp::GetAccessPoints()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ap_records_;
}   

WifiConfigurationAp::~WifiConfigurationAp()
{
    Stop();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void WifiConfigurationAp::SetLanguage(const std::string &&language)
{
    language_ = language;
}

void WifiConfigurationAp::SetLanguage(const std::string &language)
{
    language_ = language;
}

void WifiConfigurationAp::SetSsidPrefix(const std::string &&ssid_prefix)
{
    ssid_prefix_ = ssid_prefix;
}

void WifiConfigurationAp::SetSsidPrefix(const std::string &ssid_prefix)
{
    ssid_prefix_ = ssid_prefix;
}

void WifiConfigurationAp::Start()
{
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiConfigurationAp::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiConfigurationAp::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));

    StartAccessPoint();
    StartWebServer();
    
    // Start scan immediately
    esp_wifi_scan_start(nullptr, false);
    // Setup periodic WiFi scan timer.
    // skip_unhandled_events = false so the timer can wake the CPU from light
    // sleep on its own; otherwise the AP-mode scan list would stop refreshing
    // whenever the user paused interacting with the config web UI. See
    // esp_timer_get_next_alarm_for_wake_up in components/esp_timer/src/esp_timer.c.
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* self = static_cast<WifiConfigurationAp*>(arg);
            if (!self->is_connecting_) {
                esp_wifi_scan_start(nullptr, false);
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_scan_timer",
        .skip_unhandled_events = false
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &scan_timer_));
}

std::string WifiConfigurationAp::GetSsid()
{
    // Get MAC and use it to generate a unique SSID
    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    esp_wifi_get_mac(WIFI_IF_AP, mac);
#else
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
#endif
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", ssid_prefix_.c_str(), mac[4], mac[5]);
    return std::string(ssid);
}

std::string WifiConfigurationAp::GetWebServerUrl()
{
    // http://192.168.4.1
    return "http://192.168.4.1";
}

void WifiConfigurationAp::StartAccessPoint()
{
    // Note: esp_netif_init() and esp_wifi_init() should be called once before calling this method
    // WiFi driver is initialized by WifiManager::Initialize() and kept alive
    
    // WifiManager initializes the driver but owns no netifs. The normal
    // station is stopped (and its netif destroyed) before AP mode starts.
    // APSTA needs its own STA netif so compatibility provisioning callers
    // waiting for GOT_IP can actually run DHCP.
    station_netif_ = esp_netif_create_default_wifi_sta();
    ap_netif_ = esp_netif_create_default_wifi_ap();

    // Set the router IP address to 192.168.4.1
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif_);
    esp_netif_set_ip_info(ap_netif_, &ip_info);
    esp_netif_dhcps_start(ap_netif_);

    // Start the DNS server
    dns_server_ = std::make_unique<DnsServer>();
    dns_server_->Start(ip_info.gw);

    // Get the SSID
    std::string ssid = GetSsid();

    // Set the WiFi configuration
    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.ap.ssid, ssid.c_str());
    wifi_config.ap.ssid_len = ssid.length();
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    // Start the WiFi Access Point
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
#else
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY));
#endif

    ESP_LOGI(TAG, "Access Point started with SSID %s", ssid.c_str());

    // 加载高级配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        // 读取OTA URL
        char ota_url[256] = {0};
        size_t ota_url_size = sizeof(ota_url);
        err = nvs_get_str(nvs, "ota_url", ota_url, &ota_url_size);
        if (err == ESP_OK) {
            ota_url_ = ota_url;
        }

        // 读取WiFi功率
        err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "WiFi max tx power from NVS: %d", max_tx_power_);
            ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
        } else {
            esp_wifi_get_max_tx_power(&max_tx_power_);
        }

        // 读取BSSID记忆设置
        uint8_t remember_bssid = 0;
        err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid);
        if (err == ESP_OK) {
            remember_bssid_ = remember_bssid != 0;
        } else {
            remember_bssid_ = false; // 默认值
        }

        // 读取睡眠模式设置
        uint8_t sleep_mode = 0;
        err = nvs_get_u8(nvs, "sleep_mode", &sleep_mode);
        if (err == ESP_OK) {
            sleep_mode_ = sleep_mode != 0;
        } else {
            sleep_mode_ = true; // 默认值
        }

        nvs_close(nvs);
    }
}

void WifiConfigurationAp::StartWebServer()
{
    // Start the web server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 25;
    config.uri_match_fn = httpd_uri_match_wildcard;
    // 5G Network takes longer to connect
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    ESP_ERROR_CHECK(httpd_start(&server_, &config));

    // Register the index.html file
    httpd_uri_t index_html = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, index_html_start, strlen(index_html_start));
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &index_html));

    // Register the /saved/list URI
    httpd_uri_t saved_list = {
        .uri = "/saved/list",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            const auto ssid_list = SsidManager::GetInstance().GetSsidList();
            std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_CreateArray(), cJSON_Delete);
            if (!json) return SendWifiListJson(req, nullptr);
            for (const auto& ssid : ssid_list) {
                std::unique_ptr<cJSON, decltype(&cJSON_Delete)> item(cJSON_CreateString(ssid.ssid.c_str()), cJSON_Delete);
                if (!item || !cJSON_AddItemToArray(json.get(), item.get())) {
                    return SendWifiListJson(req, nullptr);
                }
                item.release(); // The array owns this string after attachment.
            }
            return SendWifiListJson(req, json.get());
        },
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &saved_list));

    // Save button: persist credentials without connecting or exiting AP mode.
    httpd_uri_t saved_add = {
        .uri = "/saved/add",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            if (req->content_len == 0 || req->content_len > 1024) {
                httpd_resp_set_status(req, "400 Bad Request");
                return httpd_resp_send(req, "{\"success\":false,\"error\":\"Invalid request size\"}", HTTPD_RESP_USE_STRLEN);
            }
            char body[1025];
            size_t received = 0;
            while (received < req->content_len) {
                const int result = httpd_req_recv(req, body + received, req->content_len - received);
                if (result <= 0) {
                    httpd_resp_set_status(req, result == HTTPD_SOCK_ERR_TIMEOUT ? "408 Request Timeout" : "400 Bad Request");
                    httpd_resp_send(req, "{\"success\":false,\"error\":\"Incomplete request body\"}", HTTPD_RESP_USE_STRLEN);
                    return ESP_FAIL;
                }
                received += result;
            }
            body[received] = '\0';
            std::string ssid;
            std::string password;
            if (!ParseSavedWifiRequest(body, received, ssid, password)) {
                httpd_resp_set_status(req, "400 Bad Request");
                return httpd_resp_send(req, "{\"success\":false,\"error\":\"Invalid Wi-Fi credentials JSON\"}", HTTPD_RESP_USE_STRLEN);
            }
            auto *this_ = static_cast<WifiConfigurationAp *>(req->user_ctx);
            if (!this_->Save(ssid, password)) {
                httpd_resp_set_status(req, "500 Internal Server Error");
                return httpd_resp_send(req, "{\"success\":false,\"error\":\"Failed to save Wi-Fi credentials\"}", HTTPD_RESP_USE_STRLEN);
            }
            return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &saved_add));

    // Register the /saved/set_default URI
    httpd_uri_t saved_set_default = {
        .uri = "/saved/set_default",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            std::string uri = req->uri;
            auto pos = uri.find("?index=");
            if (pos != std::string::npos) {
                int index = -1;
                sscanf(&req->uri[pos+7], "%d", &index);
                ESP_LOGI(TAG, "Set default item %d", index);
                SsidManager::GetInstance().SetDefaultSsid(index);
            }
            // send {}
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &saved_set_default));

    // Register the /saved/delete URI
    httpd_uri_t saved_delete = {
        .uri = "/saved/delete",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            const std::string uri = req->uri;
            const auto pos = uri.find("?index=");
            int index = -1;
            if (pos == std::string::npos) {
                httpd_resp_set_status(req, "400 Bad Request");
                return httpd_resp_send(req, "{\"success\":false,\"error\":\"Missing index\"}", HTTPD_RESP_USE_STRLEN);
            }
            const char* begin = uri.data() + pos + 7;
            const char* end = uri.data() + uri.size();
            const auto parsed = std::from_chars(begin, end, index);
            auto& manager = SsidManager::GetInstance();
            if (parsed.ec != std::errc() || parsed.ptr != end || index < 0 ||
                static_cast<size_t>(index) >= manager.GetSsidList().size()) {
                httpd_resp_set_status(req, "400 Bad Request");
                return httpd_resp_send(req, "{\"success\":false,\"error\":\"Invalid index\"}", HTTPD_RESP_USE_STRLEN);
            }
            if (!manager.RemoveSsid(index)) {
                httpd_resp_set_status(req, "500 Internal Server Error");
                return httpd_resp_send(req, "{\"success\":false,\"error\":\"Failed to delete saved network\"}", HTTPD_RESP_USE_STRLEN);
            }
            return httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
        },
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &saved_delete));

    // Register the /scan URI
    httpd_uri_t scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            auto *this_ = static_cast<WifiConfigurationAp *>(req->user_ctx);
            // Release the scan mutex before encoding or performing network I/O.
            const auto ap_records = this_->GetAccessPoints();

            // Check if 5G is supported
            bool support_5g = false;
#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
            support_5g = true;
#endif

            std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_CreateObject(), cJSON_Delete);
            if (!json || !cJSON_AddBoolToObject(json.get(), "support_5g", support_5g)) {
                return SendWifiListJson(req, nullptr);
            }
            cJSON* aps = cJSON_AddArrayToObject(json.get(), "aps");
            if (!aps) return SendWifiListJson(req, nullptr);
            for (const auto& ap : ap_records) {
                const std::string ssid = WifiSsidFromBytes(ap.ssid);
                std::unique_ptr<cJSON, decltype(&cJSON_Delete)> item(cJSON_CreateObject(), cJSON_Delete);
                if (!item || !cJSON_AddStringToObject(item.get(), "ssid", ssid.c_str()) ||
                    !cJSON_AddNumberToObject(item.get(), "rssi", ap.rssi) ||
                    !cJSON_AddNumberToObject(item.get(), "authmode", ap.authmode) ||
                    !cJSON_AddItemToArray(aps, item.get())) {
                    return SendWifiListJson(req, nullptr);
                }
                item.release(); // The root now owns the complete AP object.
            }
            return SendWifiListJson(req, json.get());
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &scan));

    // Register the form submission
    httpd_uri_t form_submit = {
        .uri = "/submit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            std::string body;
            if (!ReadProvisioningBody(req, body)) return ESP_FAIL;
            std::string ssid;
            std::string password;
            if (!ParseSavedWifiRequest(body.data(), body.size(), ssid, password)) {
                return ProvisioningFailure(req, "400 Bad Request", "Invalid Wi-Fi credentials JSON");
            }
            auto *self = static_cast<WifiConfigurationAp *>(req->user_ctx);
            if (self->exit_pending_) {
                return ProvisioningFailure(req, "409 Conflict", "Configuration was already saved; device is restarting");
            }
            TaskHandle_t exit_task = nullptr;
            if (!PrepareProvisioningExitTask(&self->on_exit_requested_, &exit_task)) {
                return ProvisioningFailure(req, "503 Service Unavailable", "Failed to schedule configuration restart");
            }
            // Never connect the STA from the HTTP task: APSTA channel changes
            // can disconnect the phone before it receives the response.
            if (!self->Save(ssid, password)) {
                vTaskDelete(exit_task);
                return ProvisioningFailure(req, "500 Internal Server Error", "Failed to save Wi-Fi credentials");
            }
            self->exit_pending_ = true;
            const auto sent = httpd_resp_send(req,
                "{\"success\":true,\"saved\":true,\"restarting\":true}", HTTPD_RESP_USE_STRLEN);
            // Durable credentials must be used even if the response is lost.
            xTaskNotifyGive(exit_task);
            return sent;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &form_submit));

    // Register the done.html page
    httpd_uri_t done_html = {
        .uri = "/done.html",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, done_html_start, strlen(done_html_start));
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &done_html));

    // The exit callback runs in a separate task only after the response was
    // queued successfully. The board callback can reboot without killing the
    // HTTP server in the middle of its own handler.
    httpd_uri_t exit_config = {
        .uri = "/exit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            auto* self = static_cast<WifiConfigurationAp*>(req->user_ctx);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "Connection", "close");
            if (self->exit_pending_) {
                return httpd_resp_send(req, "{\"success\":true,\"restarting\":true}", HTTPD_RESP_USE_STRLEN);
            }
            TaskHandle_t exit_task = nullptr;
            if (!PrepareProvisioningExitTask(&self->on_exit_requested_, &exit_task)) {
                return ProvisioningFailure(req, "503 Service Unavailable", "Failed to schedule configuration exit");
            }
            const auto sent = httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
            if (sent == ESP_OK) {
                self->exit_pending_ = true;
                xTaskNotifyGive(exit_task);
            } else vTaskDelete(exit_task);
            return sent;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &exit_config));

    auto captive_portal_handler = [](httpd_req_t *req) -> esp_err_t {
        auto *this_ = static_cast<WifiConfigurationAp *>(req->user_ctx);
        std::string url = this_->GetWebServerUrl() + "/?lang=" + this_->language_ + "&_=" + std::to_string(esp_timer_get_time());
        // Set content type to prevent browser warnings
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", url.c_str());
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    };

    // Register all common captive portal detection endpoints
    const char* captive_portal_urls[] = {
        "/hotspot-detect.html",    // Apple
        "/generate_204*",           // Android
        "/mobile/status.php",      // Android
        "/check_network_status.txt", // Windows
        "/ncsi.txt",              // Windows
        "/fwlink/",               // Microsoft
        "/connectivity-check.html", // Firefox
        "/success.txt",           // Various
        "/portal.html",           // Various
        "/library/test/success.html" // Apple
    };

    for (const auto& url : captive_portal_urls) {
        httpd_uri_t redirect_uri = {
            .uri = url,
            .method = HTTP_GET,
            .handler = captive_portal_handler,
            .user_ctx = this
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &redirect_uri));
    }

    // Register the /advanced/config URI
    httpd_uri_t advanced_config = {
        .uri = "/advanced/config",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            // 获取当前对象
            auto *this_ = static_cast<WifiConfigurationAp *>(req->user_ctx);
            
            // 创建JSON对象
            cJSON *json = cJSON_CreateObject();
            if (!json) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON");
                return ESP_FAIL;
            }

            // 添加配置项到JSON
            if (!this_->ota_url_.empty()) {
                cJSON_AddStringToObject(json, "ota_url", this_->ota_url_.c_str());
            }
            cJSON_AddStringToObject(json, "connection_failure", this_->connection_failure_.c_str());
            cJSON_AddNumberToObject(json, "max_tx_power", this_->max_tx_power_);
            cJSON_AddBoolToObject(json, "remember_bssid", this_->remember_bssid_);
            cJSON_AddBoolToObject(json, "sleep_mode", this_->sleep_mode_);

            // 发送JSON响应
            char *json_str = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            if (!json_str) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to print JSON");
                return ESP_FAIL;
            }

            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, json_str, strlen(json_str));
            free(json_str);
            return ESP_OK;
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &advanced_config));

    // Register the /advanced/submit URI
    httpd_uri_t advanced_submit = {
        .uri = "/advanced/submit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            std::string body;
            if (!ReadProvisioningBody(req, body)) return ESP_FAIL;
            auto json = ParseProvisioningJson(body.data(), body.size());
            if (!json) return ProvisioningFailure(req, "400 Bad Request", "Invalid configuration JSON");
            const cJSON* ota_url = nullptr;
            const cJSON* max_tx_power = nullptr;
            const cJSON* remember_bssid = nullptr;
            const cJSON* sleep_mode = nullptr;
            for (const cJSON* item = json->child; item; item = item->next) {
                if (strcmp(item->string, "ota_url") == 0 && cJSON_IsString(item) && item->valuestring &&
                    IsProvisioningOtaUrlValid(item->valuestring)) ota_url = item;
                else if (strcmp(item->string, "max_tx_power") == 0 && cJSON_IsNumber(item) &&
                         item->valuedouble == item->valueint && item->valueint >= 8 && item->valueint <= 80) max_tx_power = item;
                else if (strcmp(item->string, "remember_bssid") == 0 && cJSON_IsBool(item)) remember_bssid = item;
                else if (strcmp(item->string, "sleep_mode") == 0 && cJSON_IsBool(item)) sleep_mode = item;
                else return ProvisioningFailure(req, "400 Bad Request", "Invalid configuration field");
            }
            auto *self = static_cast<WifiConfigurationAp *>(req->user_ctx);
            if (ota_url) {
                const auto error = SaveProvisioningOtaUrl(ota_url->valuestring);
                if (error != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to save OTA configuration: %s", esp_err_to_name(error));
                    return ProvisioningFailure(req, "500 Internal Server Error", "Failed to save OTA configuration");
                }
                self->ota_url_ = ota_url->valuestring;
            }
            if (max_tx_power || remember_bssid || sleep_mode) {
                nvs_handle_t handle = 0;
                auto error = nvs_open("wifi", NVS_READWRITE, &handle);
                if (error == ESP_OK && max_tx_power) error = nvs_set_i8(handle, "max_tx_power", max_tx_power->valueint);
                if (error == ESP_OK && remember_bssid) error = nvs_set_u8(handle, "remember_bssid", cJSON_IsTrue(remember_bssid));
                if (error == ESP_OK && sleep_mode) error = nvs_set_u8(handle, "sleep_mode", cJSON_IsTrue(sleep_mode));
                if (error == ESP_OK) error = nvs_commit(handle);
                if (handle) nvs_close(handle);
                if (error != ESP_OK) return ProvisioningFailure(req, "500 Internal Server Error", "Failed to save Wi-Fi settings");
                if (max_tx_power) {
                    self->max_tx_power_ = max_tx_power->valueint;
                    error = esp_wifi_set_max_tx_power(self->max_tx_power_);
                    if (error != ESP_OK) return ProvisioningFailure(req, "500 Internal Server Error", "Failed to apply Wi-Fi power");
                }
                if (remember_bssid) self->remember_bssid_ = cJSON_IsTrue(remember_bssid);
                if (sleep_mode) self->sleep_mode_ = cJSON_IsTrue(sleep_mode);
            }
            return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
        },
        .user_ctx = this
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &advanced_submit));

    ESP_LOGI(TAG, "Web server started");
}

bool WifiConfigurationAp::ConnectToWifi(const std::string &ssid, const std::string &password)
{
    if (ssid.empty()) {
        ESP_LOGE(TAG, "SSID cannot be empty");
        return false;
    }
    
    if (ssid.length() > 32) {  // WiFi SSID 最大长度
        ESP_LOGE(TAG, "SSID too long");
        return false;
    }

    if (password.length() > 64) {
        ESP_LOGE(TAG, "Password too long");
        return false;
    }
    
    is_connecting_ = true;

    // Upper-level retry loop with delay between attempts.
    //
    // Background: in APSTA mode the captive-portal session is on the AP
    // beacon channel (typically 1), and the target home AP is on some
    // other channel (e.g. 10). When ConnectToWifi triggers, esp-wifi
    // performs a Channel Switch Announcement to move both AP and STA to
    // the home AP's channel, then immediately issues an association
    // request. The home AP frequently responds with "Association
    // Response status=30 (Refused Temporarily)" + a Comeback Time in
    // TUs (= ~1.1s for Buffalo routers, observed) because its own
    // state hasn't settled yet for the new station.
    //
    // The ESP-IDF wifi driver's failure_retry_cnt issues re-association
    // attempts back-to-back (within a few ms) and does not honor the
    // 802.11 Comeback Time, so every driver-internal retry is refused
    // the same way and the first ConnectToWifi() call returns failure.
    // By the time the user clicks "submit" a second time (~8s later)
    // the AP has fully settled and association succeeds on the first
    // try — which is why users observe "it always fails the first
    // time, then works".
    //
    // Fix: when an attempt fails, wait long enough for the comeback
    // timer + AP state settle (~3s is safe), then retry once. This
    // produces a single user-visible success path instead of forcing
    // the user to resubmit. The driver-internal retries (set below
    // via failure_retry_cnt) are kept as a secondary safety net.
    constexpr int kMaxAttempts = 2;
    constexpr int kRetryDelayMs = 3000;
    bool connected = false;

    for (int attempt = 1; attempt <= kMaxAttempts && !connected; ++attempt) {
        if (attempt > 1) {
            ESP_LOGI(TAG,
                "WiFi attempt %d/%d after %d ms delay "
                "(waiting for AP comeback timer + state settle)",
                attempt, kMaxAttempts, kRetryDelayMs);
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
        }

        xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        esp_wifi_scan_stop();

        wifi_config_t wifi_config;
        bzero(&wifi_config, sizeof(wifi_config));
        if (!CopyWifiCredentials(ssid, password, wifi_config.sta.ssid, wifi_config.sta.password)) {
            is_connecting_ = false;
            return false;
        }
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config.sta.failure_retry_cnt = 1;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        auto ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG,
                "esp_wifi_connect failed: %d (attempt %d/%d)",
                ret, attempt, kMaxAttempts);
            continue;
        }
        ESP_LOGI(TAG, "Connecting to WiFi %s (attempt %d/%d)",
            ssid.c_str(), attempt, kMaxAttempts);

        // Wait for the connection to complete for 10 or 25 seconds.
        EventBits_t bits = xEventGroupWaitBits(
            event_group_,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdTRUE,
            pdFALSE,
#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
            pdMS_TO_TICKS(25000)
#else
            pdMS_TO_TICKS(10000)
#endif
        );

        if (bits & WIFI_CONNECTED_BIT) {
            connected = true;
        } else {
            const bool timed_out = (bits == 0);
            if (timed_out) {
                // Timeout — neither WIFI_CONNECTED_BIT nor WIFI_FAIL_BIT
                // was set, so WIFI_EVENT_STA_DISCONNECTED has not fired
                // and the driver may still be in `connecting` state.
                // Cancel the in-flight attempt explicitly before the
                // retry delay; without this the next esp_wifi_connect()
                // can return ESP_ERR_WIFI_STATE on a connecting-state
                // driver (per esp_wifi.h attention 3), making the retry
                // a no-op on slow / event-dropping APs.
                esp_wifi_disconnect();
            }
            ESP_LOGW(TAG,
                "Attempt %d/%d %s%s",
                attempt, kMaxAttempts,
                timed_out ? "timed out (driver may still be connecting)" : "failed",
                attempt < kMaxAttempts ? " — will retry" : "");
        }
    }
    is_connecting_ = false;

    if (connected) {
        ESP_LOGI(TAG, "Connected to WiFi %s", ssid.c_str());
        esp_wifi_disconnect();
        return true;
    } else {
        ESP_LOGE(TAG,
            "Failed to connect to WiFi %s after %d attempts",
            ssid.c_str(), kMaxAttempts);
        return false;
    }
}

bool WifiConfigurationAp::Save(const std::string &ssid, const std::string &password)
{
    ESP_LOGI(TAG, "Save SSID %s %d", ssid.c_str(), ssid.length());
    return SsidManager::GetInstance().AddSsid(ssid, password);
}

void WifiConfigurationAp::OnExitRequested(std::function<void()> callback)
{
    on_exit_requested_ = callback;
}

void WifiConfigurationAp::SetConnectionFailure(const std::string& message)
{
    connection_failure_ = message;
}

void WifiConfigurationAp::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    WifiConfigurationAp* self = static_cast<WifiConfigurationAp*>(arg);
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(self->event_group_, WIFI_FAIL_BIT);
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        std::lock_guard<std::mutex> lock(self->mutex_);
        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);

        self->ap_records_.resize(ap_num);
        esp_wifi_scan_get_ap_records(&ap_num, self->ap_records_.data());

        // 扫描完成，等待10秒后再次扫描
        esp_timer_start_once(self->scan_timer_, 10 * 1000000);
    }
}

void WifiConfigurationAp::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    WifiConfigurationAp* self = static_cast<WifiConfigurationAp*>(arg);
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(self->event_group_, WIFI_CONNECTED_BIT);
    }
}

#if !CONFIG_IDF_TARGET_ESP32P4
void WifiConfigurationAp::StartSmartConfig()
{
    // 注册SmartConfig事件处理器
    ESP_ERROR_CHECK(esp_event_handler_instance_register(SC_EVENT, ESP_EVENT_ANY_ID,
                                                        &WifiConfigurationAp::SmartConfigEventHandler, this, &sc_event_instance_));

    // 初始化SmartConfig配置
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    // cfg.esp_touch_v2_enable_crypt = true;
    // cfg.esp_touch_v2_key = "1234567890123456"; // 设置16字节加密密钥

    // 启动SmartConfig服务
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
    ESP_LOGI(TAG, "SmartConfig started");
}

void WifiConfigurationAp::SmartConfigEventHandler(void *arg, esp_event_base_t event_base,
                                                  int32_t event_id, void *event_data)
{
    WifiConfigurationAp *self = static_cast<WifiConfigurationAp *>(arg);

    if (event_base == SC_EVENT){
        switch (event_id){
        case SC_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "SmartConfig scan done");
            break;
        case SC_EVENT_FOUND_CHANNEL:
            ESP_LOGI(TAG, "Found SmartConfig channel");
            break;
        case SC_EVENT_GOT_SSID_PSWD:{
            ESP_LOGI(TAG, "Got SmartConfig credentials");
            smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;

            char ssid[33] = {}, password[65] = {};
            memcpy(ssid, evt->ssid, sizeof(evt->ssid));
            memcpy(password, evt->password, sizeof(evt->password));
            ESP_LOGI(TAG, "SmartConfig SSID: %s, Password: %s", ssid, password);
            // 尝试连接WiFi会失败，故不连接
            if (!self->Save(ssid, password)) {
                ESP_LOGE(TAG, "SmartConfig credentials could not be saved");
                break;
            }
            // 延迟退出配网模式
            xTaskCreate([](void *ctx){
                ESP_LOGI(TAG, "Exiting config mode in 1 second");
                vTaskDelay(pdMS_TO_TICKS(1000));
                auto* self = static_cast<WifiConfigurationAp*>(ctx);
                if (self->on_exit_requested_) {
                    self->on_exit_requested_();
                }
                vTaskDelete(NULL);
            }, "exit_config_task", 4096, self, 5, NULL);
            break;
        }
        case SC_EVENT_SEND_ACK_DONE:
            ESP_LOGI(TAG, "SmartConfig ACK sent");
            esp_smartconfig_stop();
            break;
        }
    }
}
#endif // !CONFIG_IDF_TARGET_ESP32P4

void WifiConfigurationAp::Stop() {
#if !CONFIG_IDF_TARGET_ESP32P4
    // 停止SmartConfig服务
    if (sc_event_instance_) {
        esp_event_handler_instance_unregister(SC_EVENT, ESP_EVENT_ANY_ID, sc_event_instance_);
        sc_event_instance_ = nullptr;
    }
    esp_smartconfig_stop();
#endif

    // 停止定时器
    if (scan_timer_) {
        esp_timer_stop(scan_timer_);
        esp_timer_delete(scan_timer_);
        scan_timer_ = nullptr;
    }

    // 停止Web服务器
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }

    // 停止DNS服务器
    if (dns_server_) {
        dns_server_->Stop();
        dns_server_.reset();
    }

    // 注销事件处理器
    if (instance_any_id_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }

    // 停止WiFi（但不 deinit，WiFi 驱动由 WifiManager 管理）
    esp_wifi_stop();
    
    // 销毁网络接口
    if (ap_netif_) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }
    if (station_netif_) {
        esp_netif_destroy_default_wifi(station_netif_);
        station_netif_ = nullptr;
    }

    ESP_LOGI(TAG, "Wifi configuration AP stopped");
}
