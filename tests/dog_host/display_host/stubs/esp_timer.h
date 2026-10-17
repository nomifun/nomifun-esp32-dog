#pragma once
#include <cstdint>
#include <lvgl.h>
using esp_timer_handle_t = void*;
constexpr int ESP_OK = 0;
constexpr int ESP_TIMER_TASK = 0;
struct esp_timer_create_args_t {
    void (*callback)(void*); void* arg; int dispatch_method; const char* name; bool skip_unhandled_events;
};
inline int esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t* out) { *out = reinterpret_cast<void*>(1); return ESP_OK; }
inline int esp_timer_start_periodic(esp_timer_handle_t, uint64_t) { return ESP_OK; }
inline int esp_timer_stop(esp_timer_handle_t) { return ESP_OK; }
inline int esp_timer_delete(esp_timer_handle_t) { return ESP_OK; }
inline int64_t esp_timer_get_time() { return int64_t(lv_tick_get()) * 1000; }
