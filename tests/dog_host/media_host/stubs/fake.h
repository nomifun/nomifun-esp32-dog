#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace host {
inline time_t now = 0;
inline int64_t micros = 0;
inline int iterations = 0;
inline bool fail_commit = false, fail_decode = false, fail_acquire = false;
inline int commits = 0, writes = 0, begins = 0, decoder_opens = 0;
inline std::map<std::string, std::string> persisted;
inline std::map<uint32_t, std::map<std::string, std::string>> pending;
inline std::function<void()> on_delay, on_cancel;
struct EndIteration {};
}
inline time_t media_host_time(time_t* out) { if (out) *out = host::now; return host::now; }
inline tm* localtime_r(const time_t* value, tm* out) { return localtime_s(out, value) == 0 ? out : nullptr; }

using TaskHandle_t = void*;
constexpr int pdPASS = 1, pdTRUE = 1;
#define pdMS_TO_TICKS(value) (value)
inline int xTaskCreate(void (*)(void*), const char*, int, void*, int, TaskHandle_t* out) {
    *out = reinterpret_cast<void*>(1); return pdPASS;
}
inline void vTaskDelete(void*) {}
inline void xTaskNotifyGive(TaskHandle_t) {}
inline void vTaskDelay(int ms) { host::micros += ms * 1000; if (host::on_delay) host::on_delay(); }
inline int ulTaskNotifyTake(int, int) {
    if (host::iterations-- <= 0) throw host::EndIteration{};
    return 1;
}
inline int64_t esp_timer_get_time() { return host::micros += 1000; }
inline uint32_t esp_random() { return 0; }
using esp_err_t = int;
constexpr int ESP_OK = 0;
inline const char* esp_err_to_name(int) { return "HOST_WRITE_FAILED"; }
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)

using nvs_handle_t = uint32_t;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
inline int nvs_open(const char*, int, nvs_handle_t* out) { static uint32_t next = 1; *out = next++; return 0; }
inline void nvs_close(nvs_handle_t handle) { host::pending.erase(handle); }
inline int nvs_commit(nvs_handle_t handle) {
    ++host::commits;
    if (host::fail_commit) return -1;
    for (auto& [key, value] : host::pending[handle]) host::persisted[key] = value;
    return 0;
}
inline int nvs_set_i32(nvs_handle_t h, const char* key, int32_t value) { host::pending[h][key] = std::to_string(value); return 0; }
inline int nvs_set_u8(nvs_handle_t h, const char* key, uint8_t value) { return nvs_set_i32(h, key, value); }
inline int nvs_set_str(nvs_handle_t h, const char* key, const char* value) { host::pending[h][key] = value; return 0; }
inline int nvs_get_i32(nvs_handle_t, const char* key, int32_t* value) {
    auto it = host::persisted.find(key); if (it == host::persisted.end()) return -1;
    *value = std::stoi(it->second); return 0;
}
inline int nvs_get_u8(nvs_handle_t h, const char* key, uint8_t* value) {
    int32_t result; int err = nvs_get_i32(h, key, &result); if (!err) *value = static_cast<uint8_t>(result); return err;
}
inline int nvs_get_str(nvs_handle_t, const char* key, char* value, size_t* size) {
    auto it = host::persisted.find(key); if (it == host::persisted.end()) return -1;
    if (value) { if (*size < it->second.size() + 1) return -1; memcpy(value, it->second.c_str(), it->second.size() + 1); }
    *size = it->second.size() + 1; return 0;
}
struct Settings {
    explicit Settings(const std::string&, bool = false) {}
    int GetInt(const std::string&, int fallback = 0) { return fallback; }
};
enum DeviceState {
    kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking, kDeviceStateWifiConfiguring,
    kDeviceStateActivating, kDeviceStateUpgrading, kDeviceStateConnecting
};
struct AudioCodec {
    int output_channels() const { return 1; }
    int output_sample_rate() const { return 16000; }
};
struct AudioService {
    bool active = false;
    bool BeginLocalPlayback() { ++host::begins; if (host::fail_acquire) return false; active = true; return true; }
    bool WriteLocalPlayback(std::vector<int16_t>&& samples) { if (!active || samples.empty()) return false; ++host::writes; return true; }
    void EndLocalPlayback() { active = false; }
    void CancelLocalPlayback() { active = false; if (host::on_cancel) host::on_cancel(); }
    bool IsIdle() const { return !active; }
};
struct Application {
    DeviceState state = kDeviceStateIdle;
    AudioService audio;
    static Application& GetInstance() { static Application app; return app; }
    DeviceState GetDeviceState() const { return state; }
    void StopListening() { state = kDeviceStateIdle; }
    void Schedule(std::function<void()> call) { call(); }
    AudioService& GetAudioService() { return audio; }
};
struct Board {
    AudioCodec codec;
    static Board& GetInstance() { static Board board; return board; }
    AudioCodec* GetAudioCodec() { return &codec; }
};
constexpr int ESP_AUDIO_ERR_OK = 0, ESP_AUDIO_ERR_ALREADY_EXIST = 1, ESP_AUDIO_ERR_BUFF_NOT_ENOUGH = 2;
constexpr int ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 = 1;
struct esp_audio_simple_dec_cfg_t { int dec_type; bool use_frame_dec; };
using esp_audio_simple_dec_handle_t = void*;
struct esp_audio_simple_dec_raw_t { uint8_t* buffer; size_t len; bool eos; size_t consumed; };
struct esp_audio_simple_dec_out_t { uint8_t* buffer; size_t len, needed_size, decoded_size; };
struct esp_audio_simple_dec_info_t { int bits_per_sample, channel, sample_rate; };
inline int esp_mp3_dec_register() { return 0; }
inline int esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t*, void** out) {
    ++host::decoder_opens; *out = reinterpret_cast<void*>(1); return 0;
}
inline void esp_audio_simple_dec_close(void*) {}
inline int esp_audio_simple_dec_process(void*, esp_audio_simple_dec_raw_t* raw, esp_audio_simple_dec_out_t* out) {
    if (host::fail_decode) return -1;
    raw->consumed = raw->len;
    out->decoded_size = 8;
    const int16_t pcm[] = {0, 300, -300, 0}; memcpy(out->buffer, pcm, sizeof(pcm)); return 0;
}
inline int esp_audio_simple_dec_get_info(void*, esp_audio_simple_dec_info_t* info) { *info = {16, 1, 8000}; return 0; }
