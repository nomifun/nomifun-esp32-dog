#pragma once
#include <stdexcept>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_READ_ONLY = 0x1104;
constexpr esp_err_t ESP_ERR_NVS_INVALID_HANDLE = 0x1107;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 0x110c;
inline const char* esp_err_to_name(esp_err_t) { return "host NVS error"; }
inline void HostErrorCheck(esp_err_t error) {
    if (error != ESP_OK) throw std::runtime_error("ESP_ERROR_CHECK failed");
}
#define ESP_ERROR_CHECK(expression) HostErrorCheck(expression)
