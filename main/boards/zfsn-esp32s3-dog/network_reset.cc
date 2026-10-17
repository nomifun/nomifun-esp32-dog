#include "network_reset.h"

#include <esp_log.h>
#include <nvs.h>

namespace {

constexpr const char* kTag = "DogNetworkReset";

class NvsHandle {
public:
    ~NvsHandle() {
        if (value != 0) {
            nvs_close(value);
        }
    }
    nvs_handle_t value = 0;
};

esp_err_t ReportFailure(const char* ns, const char* operation, esp_err_t error) {
    ESP_LOGE(kTag, "%s %s failed: %s", ns, operation, esp_err_to_name(error));
    return error;
}

esp_err_t ClearExistingNamespace(const char* ns) {
    // Opening READWRITE would create an absent namespace. Probe first so a
    // robot that has never used a protocol does not gain empty NVS namespaces.
    NvsHandle probe;
    auto error = nvs_open(ns, NVS_READONLY, &probe.value);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return ReportFailure(ns, "open read", error);
    }

    NvsHandle writable;
    error = nvs_open(ns, NVS_READWRITE, &writable.value);
    if (error != ESP_OK) {
        return ReportFailure(ns, "open write", error);
    }
    error = nvs_erase_all(writable.value);
    if (error != ESP_OK) {
        return ReportFailure(ns, "erase", error);
    }
    error = nvs_commit(writable.value);
    if (error != ESP_OK) {
        return ReportFailure(ns, "commit", error);
    }
    return ESP_OK;
}

}  // namespace

esp_err_t ResetDogNetworkSettings() {
    // Remove server-provided endpoints and credentials as well as the test-only
    // endpoint. Otherwise a new OTA server can inherit a previous connection.
    constexpr const char* kConnectionNamespaces[] = {"websocket", "mqtt", "nf_usb_ws"};
    for (const char* ns : kConnectionNamespaces) {
        const auto error = ClearExistingNamespace(ns);
        if (error != ESP_OK) {
            return error;
        }
    }

    NvsHandle wifi;
    auto error = nvs_open("wifi", NVS_READWRITE, &wifi.value);
    if (error != ESP_OK) {
        return ReportFailure("wifi", "open write", error);
    }
    error = nvs_erase_all(wifi.value);
    if (error != ESP_OK) {
        return ReportFailure("wifi", "erase", error);
    }
    error = nvs_set_i32(wifi.value, "force_ap", 1);
    if (error != ESP_OK) {
        return ReportFailure("wifi", "set force_ap", error);
    }
    // Credential removal and the AP flag become durable in the same commit.
    error = nvs_commit(wifi.value);
    if (error != ESP_OK) {
        return ReportFailure("wifi", "commit", error);
    }
    ESP_LOGI(kTag, "Connection settings cleared; restart into configuration AP");
    return ESP_OK;
}
