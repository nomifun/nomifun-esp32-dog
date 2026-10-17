#ifndef PROVISIONING_SETTINGS_H_
#define PROVISIONING_SETTINGS_H_

#include <string>
#include <nvs.h>

// Empty selects the firmware's CONFIG_OTA_URL. Bound to the portal's 256-byte
// NVS read buffer, and limited to the transports supported by the HTTP client.
inline bool IsProvisioningOtaUrlValid(const std::string& url) {
    if (url.empty()) return true;
    if (url.size() > 255) return false;
    size_t authority = 0;
    if (url.compare(0, 7, "http://") == 0) authority = 7;
    else if (url.compare(0, 8, "https://") == 0) authority = 8;
    else return false;
    if (authority == url.size() || url[authority] == '/' || url[authority] == '?' || url[authority] == '#') return false;
    for (unsigned char character : url) {
        if (character <= 0x20 || character == 0x7f || character == '\\') return false;
    }
    return true;
}

// Invalidate old service credentials before publishing a new bootstrap URL.
// Each namespace is committed separately: interruption may require activation
// again, but can never send an old token to a newly selected service.
inline esp_err_t SaveProvisioningOtaUrl(const std::string& url) {
    if (!IsProvisioningOtaUrlValid(url)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t wifi = 0;
    auto error = nvs_open("wifi", NVS_READWRITE, &wifi);
    if (error != ESP_OK) return error;
    size_t size = 0;
    error = nvs_get_str(wifi, "ota_url", nullptr, &size);
    std::string previous;
    if (error == ESP_OK) {
        previous.resize(size);
        error = nvs_get_str(wifi, "ota_url", previous.data(), &size);
        if (error == ESP_OK && !previous.empty() && previous.back() == '\0') previous.pop_back();
    } else if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    if (error != ESP_OK || previous == url) {
        nvs_close(wifi);
        return error;
    }
    for (const char* name : {"websocket", "mqtt"}) {
        nvs_handle_t binding = 0;
        error = nvs_open(name, NVS_READWRITE, &binding);
        if (error == ESP_OK) {
            error = nvs_erase_all(binding);
            if (error == ESP_OK) error = nvs_commit(binding);
            nvs_close(binding);
        }
        if (error != ESP_OK) {
            nvs_close(wifi);
            return error;
        }
    }
    error = url.empty() ? nvs_erase_key(wifi, "ota_url") : nvs_set_str(wifi, "ota_url", url.c_str());
    if (error == ESP_ERR_NVS_NOT_FOUND && url.empty()) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(wifi);
    nvs_close(wifi);
    return error;
}

#endif
