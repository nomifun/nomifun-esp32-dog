#include "ssid_manager.h"

#include <algorithm>
#include <esp_log.h>
#include <nvs_flash.h>

#define TAG "SsidManager"
#define NVS_NAMESPACE "wifi"
#define MAX_WIFI_SSID_COUNT 10

SsidManager::SsidManager() {
    LoadFromNvs();
}

SsidManager::~SsidManager() {
}

void SsidManager::Clear() {
    ssid_list_.clear();
    SaveToNvs();
}

void SsidManager::LoadFromNvs() {
    ssid_list_.clear();

    // Load ssid and password from NVS from namespace "wifi"
    // ssid, ssid1, ssid2, ... ssid9
    // password, password1, password2, ... password9
    nvs_handle_t nvs_handle;
    auto ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        // The namespace doesn't exist, just return
        ESP_LOGW(TAG, "NVS namespace %s doesn't exist", NVS_NAMESPACE);
        return;
    }
    for (int i = 0; i < MAX_WIFI_SSID_COUNT; i++) {
        std::string ssid_key = "ssid";
        if (i > 0) {
            ssid_key += std::to_string(i);
        }
        std::string password_key = "password";
        if (i > 0) {
            password_key += std::to_string(i);
        }
        
        char ssid[33];
        char password[65];
        size_t length = sizeof(ssid);
        if (nvs_get_str(nvs_handle, ssid_key.c_str(), ssid, &length) != ESP_OK) {
            continue;
        }
        length = sizeof(password);
        if (nvs_get_str(nvs_handle, password_key.c_str(), password, &length) != ESP_OK) {
            continue;
        }
        ssid_list_.push_back({ssid, password});
    }
    nvs_close(nvs_handle);
}

bool SsidManager::SaveToNvs() {
    nvs_handle_t nvs_handle;
    auto error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (error != ESP_OK) return false;
    for (int i = 0; i < MAX_WIFI_SSID_COUNT && error == ESP_OK; i++) {
        std::string ssid_key = "ssid";
        if (i > 0) {
            ssid_key += std::to_string(i);
        }
        std::string password_key = "password";
        if (i > 0) {
            password_key += std::to_string(i);
        }
        
        if (i < ssid_list_.size()) {
            error = nvs_set_str(nvs_handle, ssid_key.c_str(), ssid_list_[i].ssid.c_str());
            if (error == ESP_OK) error = nvs_set_str(nvs_handle, password_key.c_str(), ssid_list_[i].password.c_str());
        } else {
            error = nvs_erase_key(nvs_handle, ssid_key.c_str());
            if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
            if (error == ESP_OK) error = nvs_erase_key(nvs_handle, password_key.c_str());
            if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
        }
    }
    if (error == ESP_OK) error = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    if (error != ESP_OK) ESP_LOGE(TAG, "Failed to save Wi-Fi credentials: %s", esp_err_to_name(error));
    return error == ESP_OK;
}

bool SsidManager::AddSsid(const std::string& ssid, const std::string& password) {
    const auto previous = ssid_list_;
    for (auto& item : ssid_list_) {
        ESP_LOGI(TAG, "compare [%s:%d] [%s:%d]", item.ssid.c_str(), item.ssid.size(), ssid.c_str(), ssid.size());
        if (item.ssid == ssid) {
            ESP_LOGW(TAG, "SSID %s already exists, overwrite it", ssid.c_str());
            item.password = password;
            if (SaveToNvs()) return true;
            ssid_list_ = previous;
            return false;
        }
    }

    if (ssid_list_.size() >= MAX_WIFI_SSID_COUNT) {
        ESP_LOGW(TAG, "SSID list is full, pop one");
        ssid_list_.pop_back();
    }
    // Add the new ssid to the front of the list
    ssid_list_.insert(ssid_list_.begin(), {ssid, password});
    if (SaveToNvs()) return true;
    ssid_list_ = previous;
    return false;
}

bool SsidManager::RemoveSsid(int index) {
    if (index < 0 || index >= ssid_list_.size()) {
        ESP_LOGW(TAG, "Invalid index %d", index);
        return false;
    }
    const auto previous = ssid_list_;
    ssid_list_.erase(ssid_list_.begin() + index);
    if (SaveToNvs()) return true;
    ssid_list_ = previous;
    return false;
}

void SsidManager::SetDefaultSsid(int index) {
    if (index < 0 || index >= ssid_list_.size()) {
        ESP_LOGW(TAG, "Invalid index %d", index);
        return;
    }
    // Move the ssid at index to the front of the list
    auto item = ssid_list_[index];
    ssid_list_.erase(ssid_list_.begin() + index);
    ssid_list_.insert(ssid_list_.begin(), item);
    SaveToNvs();
}
