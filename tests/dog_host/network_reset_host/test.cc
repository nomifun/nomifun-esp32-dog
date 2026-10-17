#include "network_reset.h"
#include "settings.h"

#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {
using Value = std::variant<std::string, std::int32_t, std::uint8_t>;
using Entries = std::map<std::string, Value>;
using Store = std::map<std::string, Entries>;
struct Handle { std::string ns; bool writable; Entries pending; };
Store durable;
std::map<nvs_handle_t, Handle> handles;
nvs_handle_t next_handle = 1;
std::string failure;
std::vector<std::string> calls;
int commits = 0;
int tests = 0;
constexpr esp_err_t kFailure = 0x2345;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool Fail(const std::string& operation, const std::string& ns) {
    const auto call = operation + ":" + ns;
    calls.push_back(call);
    return failure == call;
}

void Seed() {
    durable = {
        {"wifi", {{"ssid", std::string("old wifi")}, {"password", std::string("old secret")},
                  {"ssid1", std::string("other wifi")}, {"password1", std::string("other secret")},
                  {"ota_url", std::string("https://old.example/ota")}, {"force_ap", std::int32_t(0)},
                  {"remember_bssid", std::uint8_t(1)}, {"unknown_future_wifi_option", std::int32_t(4)}}},
        {"websocket", {{"url", std::string("wss://old.example/ws")}, {"token", std::string("old token")}}},
        {"mqtt", {{"endpoint", std::string("old.example")}, {"password", std::string("mqtt secret")}}},
        {"nf_usb_ws", {{"url", std::string("ws://127.0.0.1:18087")}, {"token", std::string("test token")}}},
        {"board", {{"uuid", std::string("preserved-uuid")}}},
        {"petdog", {{"angle_settings", std::string("preserved calibration")}}},
        {"display", {{"theme", std::string("dark")}, {"brightness", std::int32_t(75)}}},
        {"alarm", {{"alarms", std::string("preserved alarms")}}},
        {"unrelated", {{"future_value", std::string("keep")}}},
    };
    handles.clear(); next_handle = 1; calls.clear(); failure.clear(); commits = 0;
}

Store NonConnection(const Store& store) {
    auto result = store;
    for (const auto* ns : {"wifi", "websocket", "mqtt", "nf_usb_ws"}) result.erase(ns);
    return result;
}

void RequireResetResult(const Store& original) {
    Require(durable.at("wifi") == Entries{{"force_ap", std::int32_t(1)}},
            "WiFi reset must persist exactly force_ap=1 without old credentials or OTA");
    for (const auto* ns : {"websocket", "mqtt", "nf_usb_ws"}) {
        const auto it = durable.find(ns);
        Require(it == durable.end() || it->second.empty(), "Old server credentials remain");
    }
    Require(NonConnection(durable) == NonConnection(original), "Non-connection NVS data changed");
    Require(handles.empty(), "NVS handle leaked");
}

void Pass(const char* name) { ++tests; std::cout << "PASS " << name << '\n'; }
}  // namespace

esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* out) {
    if (Fail(mode == NVS_READONLY ? "open_read" : "open_write", ns)) return kFailure;
    const auto it = durable.find(ns);
    if (it == durable.end() && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    auto& entries = durable[ns];
    const auto id = next_handle++;
    handles.emplace(id, Handle{ns, mode == NVS_READWRITE, entries});
    *out = id;
    return ESP_OK;
}

void nvs_close(nvs_handle_t id) { handles.erase(id); }

esp_err_t nvs_erase_all(nvs_handle_t id) {
    const auto it = handles.find(id);
    if (it == handles.end()) return ESP_ERR_NVS_INVALID_HANDLE;
    auto& h = it->second;
    if (!h.writable) return ESP_ERR_NVS_READ_ONLY;
    if (Fail("erase", h.ns)) return kFailure;
    h.pending.clear();
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t id, const char* key) {
    const auto it = handles.find(id);
    if (it == handles.end()) return ESP_ERR_NVS_INVALID_HANDLE;
    auto& h = it->second;
    if (!h.writable) return ESP_ERR_NVS_READ_ONLY;
    if (Fail("erase_key", h.ns)) return kFailure;
    return h.pending.erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_commit(nvs_handle_t id) {
    const auto it = handles.find(id);
    if (it == handles.end()) return ESP_ERR_NVS_INVALID_HANDLE;
    auto& h = it->second;
    if (!h.writable) return ESP_ERR_NVS_READ_ONLY;
    if (Fail("commit", h.ns)) return kFailure;
    durable[h.ns] = h.pending; ++commits;
    return ESP_OK;
}

template<class T> esp_err_t SetValue(nvs_handle_t id, const char* key, T value, const char* op) {
    const auto it = handles.find(id);
    if (it == handles.end()) return ESP_ERR_NVS_INVALID_HANDLE;
    auto& h = it->second;
    if (!h.writable) return ESP_ERR_NVS_READ_ONLY;
    if (Fail(op, h.ns)) return kFailure;
    h.pending[key] = value;
    return ESP_OK;
}

template<class T> esp_err_t GetValue(nvs_handle_t id, const char* key, T* out) {
    const auto it = handles.find(id);
    if (it == handles.end()) return ESP_ERR_NVS_INVALID_HANDLE;
    const auto entry = it->second.pending.find(key);
    if (entry == it->second.pending.end() || !std::holds_alternative<T>(entry->second)) return ESP_ERR_NVS_NOT_FOUND;
    *out = std::get<T>(entry->second);
    return ESP_OK;
}

esp_err_t nvs_set_i32(nvs_handle_t id, const char* key, std::int32_t v) { return SetValue(id, key, v, "set_i32"); }
esp_err_t nvs_get_i32(nvs_handle_t id, const char* key, std::int32_t* out) { return GetValue(id, key, out); }
esp_err_t nvs_set_u8(nvs_handle_t id, const char* key, std::uint8_t v) { return SetValue(id, key, v, "set_u8"); }
esp_err_t nvs_get_u8(nvs_handle_t id, const char* key, std::uint8_t* out) { return GetValue(id, key, out); }
esp_err_t nvs_set_str(nvs_handle_t id, const char* key, const char* v) { return SetValue(id, key, std::string(v), "set_str"); }
esp_err_t nvs_get_str(nvs_handle_t id, const char* key, char* out, std::size_t* length) {
    std::string value;
    const auto error = GetValue(id, key, &value);
    if (error != ESP_OK) return error;
    const auto required = value.size() + 1;
    if (out != nullptr && *length < required) { *length = required; return ESP_ERR_NVS_INVALID_LENGTH; }
    if (out != nullptr) std::memcpy(out, value.c_str(), required);
    *length = required;
    return ESP_OK;
}

int main() {
    try {
        Seed(); auto original = durable;
        Require(ResetDogNetworkSettings() == ESP_OK, "Reset failed");
        RequireResetResult(original);
        Require(commits == 4, "Reset must commit each of four namespaces once");
        Pass("reset persists WiFi/AP together and preserves calibration identity display alarms");

        Seed(); durable.erase("websocket"); durable.erase("mqtt"); durable.erase("nf_usb_ws"); original = durable;
        Require(ResetDogNetworkSettings() == ESP_OK, "Fresh device reset failed");
        RequireResetResult(original);
        Require(durable.count("websocket") == 0 && durable.count("mqtt") == 0 && durable.count("nf_usb_ws") == 0,
                "Absent protocol namespaces must not be created");
        Require(commits == 1, "Fresh device should only commit WiFi/AP");
        Pass("absent protocol namespaces stay absent");

        Seed(); durable.erase("wifi"); original = durable;
        Require(ResetDogNetworkSettings() == ESP_OK, "Missing WiFi namespace reset failed");
        RequireResetResult(original);
        Pass("missing WiFi namespace gains durable force_ap");

        Seed(); original = durable;
        Require(ResetDogNetworkSettings() == ESP_OK && ResetDogNetworkSettings() == ESP_OK, "Repeat reset failed");
        RequireResetResult(original);
        Pass("reset is idempotent");

        std::vector<std::string> failure_points;
        for (const auto* ns : {"websocket", "mqtt", "nf_usb_ws"}) {
            for (const auto* op : {"open_read", "open_write", "erase", "commit"})
                failure_points.emplace_back(std::string(op) + ":" + ns);
        }
        for (const auto* op : {"open_write", "erase", "set_i32", "commit"})
            failure_points.emplace_back(std::string(op) + ":wifi");
        for (const auto& point : failure_points) {
            Seed(); original = durable; failure = point;
            Require(ResetDogNetworkSettings() == kFailure, "Failure must propagate and never report success");
            Require(!calls.empty() && calls.back() == point, "Reset must stop at first failed operation");
            Require(handles.empty(), "Failure leaked an NVS handle");
            Require(NonConnection(durable) == NonConnection(original), "Failure changed unrelated data");
            failure.clear();
            Require(ResetDogNetworkSettings() == ESP_OK, "Retry after failure did not recover");
            RequireResetResult(original);
            Pass(("failure propagation, stop and retry: " + point).c_str());
        }

        Seed();
        { Settings settings("wifi", true); settings.EraseKey("ota_url"); }
        Require(durable.at("wifi").count("ota_url") == 0 && commits == 1, "EraseKey was not committed");
        Pass("Settings EraseKey persists successful deletion");

        Seed(); original = durable;
        { Settings settings("wifi", true); settings.EraseKey("missing"); }
        Require(durable == original && commits == 0, "Missing key should not dirty namespace");
        Pass("Settings missing EraseKey does not commit");

        Seed();
        { Settings settings("wifi", true); settings.EraseAll(); }
        Require(durable.at("wifi").empty() && commits == 1, "EraseAll was not committed");
        Pass("Settings EraseAll persists deletion");

        Seed(); original = durable;
        { Settings settings("wifi", false); settings.EraseKey("ota_url"); settings.EraseAll(); }
        Require(durable == original && commits == 0, "Read-only erase changed storage");
        Pass("Settings read-only namespace stays unchanged");

        for (const auto* operation : {"erase_key", "erase"}) {
            Seed(); original = durable; failure = std::string(operation) + ":wifi";
            bool rejected = false;
            try {
                Settings settings("wifi", true);
                if (std::string(operation) == "erase_key") settings.EraseKey("ota_url");
                else settings.EraseAll();
            } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected, "Settings erase must retain ESP_ERROR_CHECK failure handling");
            Require(durable == original && commits == 0 && handles.empty(), "Failed Settings erase became dirty");
            Pass((std::string("Settings failure rejects without dirty commit: ") + operation).c_str());
        }

        std::cout << tests << " tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
