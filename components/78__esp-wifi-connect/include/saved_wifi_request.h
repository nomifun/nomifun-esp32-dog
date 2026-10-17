#ifndef SAVED_WIFI_REQUEST_H_
#define SAVED_WIFI_REQUEST_H_

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include "provisioning_request.h"

// Local addition to esp-wifi-connect 3.1.5 (493851d512684711fa9b6168843c112ca1f55eee).
// Credentials are persisted as NVS C strings; embedded NUL cannot be represented.
// The provisioning portal always sends exactly {"ssid": string, "password": string}.
inline bool ParseSavedWifiRequest(const char* body, size_t length,
                                  std::string& ssid, std::string& password) {
    auto root = ParseProvisioningJson(body, length);
    if (!root) return false;
    const cJSON* ssid_item = nullptr;
    const cJSON* password_item = nullptr;
    for (const cJSON* item = root->child; item; item = item->next) {
        if (!item->string || !cJSON_IsString(item) || !item->valuestring) {
            return false;
        }
        if (std::strcmp(item->string, "ssid") == 0 && !ssid_item) {
            ssid_item = item;
        } else if (std::strcmp(item->string, "password") == 0 && !password_item) {
            password_item = item;
        } else {
            return false; // Unknown or duplicate key.
        }
    }
    if (!ssid_item || !password_item) {
        return false;
    }
    const size_t ssid_length = std::strlen(ssid_item->valuestring);
    const size_t password_length = std::strlen(password_item->valuestring);
    if (ssid_length == 0 || ssid_length > 32 || password_length > 64 ||
        std::string(ssid_item->valuestring).find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
        return false;
    }
    ssid.assign(ssid_item->valuestring, ssid_length);
    password.assign(password_item->valuestring, password_length);
    return true;
}

#endif // SAVED_WIFI_REQUEST_H_
