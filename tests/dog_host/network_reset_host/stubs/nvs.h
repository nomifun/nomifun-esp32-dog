#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
using nvs_handle_t = std::uint32_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char*);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_set_i32(nvs_handle_t, const char*, std::int32_t);
esp_err_t nvs_get_i32(nvs_handle_t, const char*, std::int32_t*);
esp_err_t nvs_set_u8(nvs_handle_t, const char*, std::uint8_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char*, std::uint8_t*);
esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*);
esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, std::size_t*);
