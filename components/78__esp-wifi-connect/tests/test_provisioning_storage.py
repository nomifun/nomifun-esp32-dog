"""Run production OTA persistence/JSON helpers against real cJSON and fake NVS.

Uses the installed MSVC host compiler and no device, server, or network calls.
The fake NVS separates pending writes from committed values, and injects errors
at every persistence boundary so a failed save cannot publish a new OTA URL.
"""

import os
from pathlib import Path
import subprocess
import tempfile

import test_saved_add as host


NVS_HEADER = r'''
#pragma once
#include <cstddef>
using esp_err_t = int;
using nvs_handle_t = unsigned;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ESP_ERR_INVALID_ARG = 0x102;
constexpr int ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr int NVS_READWRITE = 1;
esp_err_t nvs_open(const char*, int, nvs_handle_t*);
esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t*);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char*);
esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
'''

SOURCE = r'''
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "provisioning_settings.h"
#include "provisioning_request.h"

using Values = std::map<std::string, std::string>;
struct Handle { std::string name; Values pending; };
std::map<std::string, Values> stored;
std::map<nvs_handle_t, Handle> handles;
std::vector<std::string> events;
std::string fail_event;
unsigned fail_occurrence = 1;
unsigned occurrence = 0;
unsigned next_handle = 1;
int cases = 0;

bool failing(const std::string& event) {
    events.push_back(event);
    return event == fail_event && ++occurrence == fail_occurrence;
}
esp_err_t nvs_open(const char* name, int, nvs_handle_t* handle) {
    if (failing(std::string("open:") + name)) return ESP_FAIL;
    *handle = next_handle++;
    handles.emplace(*handle, Handle{name, stored[name]});
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* value, size_t* size) {
    auto& h = handles.at(handle);
    if (failing("get:" + h.name + ":" + key)) return ESP_FAIL;
    const auto found = h.pending.find(key);
    if (found == h.pending.end()) return ESP_ERR_NVS_NOT_FOUND;
    const auto required = found->second.size() + 1;
    if (value) {
        if (*size < required) return ESP_FAIL;
        std::memcpy(value, found->second.c_str(), required);
    }
    *size = required;
    return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t handle) {
    auto& h = handles.at(handle);
    if (failing("erase_all:" + h.name)) return ESP_FAIL;
    h.pending.clear();
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key) {
    auto& h = handles.at(handle);
    if (failing("erase_key:" + h.name + ":" + key)) return ESP_FAIL;
    return h.pending.erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value) {
    auto& h = handles.at(handle);
    if (failing("set:" + h.name + ":" + key)) return ESP_FAIL;
    h.pending[key] = value;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    auto& h = handles.at(handle);
    if (failing("commit:" + h.name)) return ESP_FAIL;
    stored[h.name] = h.pending;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { handles.erase(handle); }

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void reset(const std::string& failure = "", unsigned instance = 1) {
    stored = {
        {"wifi", {{"ota_url", "https://old.example/ota"}, {"ssid", "Home"},
                  {"password", "secret"}, {"max_tx_power", "80"}}},
        {"websocket", {{"url", "wss://old.example/chat"}, {"token", "old-token"}}},
        {"mqtt", {{"endpoint", "old.example"}, {"password", "old-password"}}},
        {"board", {{"uuid", "stable-identity"}}},
        {"petdog", {{"angle_settings", "right-rear-adjusted"}}},
        {"display", {{"mirror", "false"}}},
        {"alarm", {{"alarms", "keep"}}},
    };
    handles.clear(); events.clear(); fail_event = failure; occurrence = 0;
    fail_occurrence = instance;
}
void preserved() {
    check(handles.empty(), "NVS handle leaked");
    check(stored["wifi"]["ssid"] == "Home", "Wi-Fi SSID changed");
    check(stored["wifi"]["password"] == "secret", "Wi-Fi password changed");
    check(stored["wifi"]["max_tx_power"] == "80", "Wi-Fi power changed");
    check(stored["board"]["uuid"] == "stable-identity", "Board identity changed");
    check(stored["petdog"]["angle_settings"] == "right-rear-adjusted", "Leg calibration changed");
    check(stored["display"]["mirror"] == "false", "Display settings changed");
    check(stored["alarm"]["alarms"] == "keep", "Alarm settings changed");
}
size_t event_index(const std::string& name) {
    auto found = std::find(events.begin(), events.end(), name);
    check(found != events.end(), "Expected persistence event missing");
    return static_cast<size_t>(found - events.begin());
}

int main() {
    reset();
    const auto original = stored;
    check(SaveProvisioningOtaUrl("https://old.example/ota") == ESP_OK, "Unchanged URL failed");
    check(stored == original, "Unchanged URL cleared credentials");
    check(std::none_of(events.begin(), events.end(), [](const std::string& e) {
        return e.rfind("erase", 0) == 0 || e.rfind("set:", 0) == 0 || e.rfind("commit:", 0) == 0;
    }), "Unchanged URL wrote NVS");
    preserved(); ++cases;

    reset();
    check(SaveProvisioningOtaUrl("http://ota.example.com/ota/") == ESP_OK, "Changed URL failed");
    check(stored["wifi"]["ota_url"] == "http://ota.example.com/ota/", "New URL not committed");
    check(stored["websocket"].empty() && stored["mqtt"].empty(), "Old cloud credentials remain");
    check(event_index("commit:websocket") < event_index("set:wifi:ota_url"), "WS not invalidated first");
    check(event_index("commit:mqtt") < event_index("set:wifi:ota_url"), "MQTT not invalidated first");
    check(event_index("set:wifi:ota_url") < event_index("commit:wifi"), "URL write not committed");
    preserved(); ++cases;

    reset();
    check(SaveProvisioningOtaUrl("") == ESP_OK, "Clearing custom URL failed");
    check(stored["wifi"].count("ota_url") == 0, "Custom URL not erased");
    check(stored["websocket"].empty() && stored["mqtt"].empty(), "Clear retained stale credentials");
    preserved(); ++cases;

    reset(); stored["wifi"].erase("ota_url");
    const auto no_custom = stored;
    check(SaveProvisioningOtaUrl("") == ESP_OK && stored == no_custom, "Already-default URL mutated state");
    preserved(); ++cases;

    reset(); stored["wifi"].erase("ota_url");
    check(SaveProvisioningOtaUrl("https://new.example/ota") == ESP_OK, "First custom URL failed");
    check(stored["websocket"].empty() && stored["mqtt"].empty(), "First custom URL retained stale credentials");
    preserved(); ++cases;

    for (const char* event : {"open:wifi", "get:wifi:ota_url", "open:websocket",
            "erase_all:websocket", "commit:websocket", "open:mqtt", "erase_all:mqtt",
            "commit:mqtt", "set:wifi:ota_url", "commit:wifi"}) {
        reset(event);
        check(SaveProvisioningOtaUrl("https://new.example/ota") != ESP_OK, "Persistence failure returned success");
        check(stored["wifi"]["ota_url"] == "https://old.example/ota", "Failed save published new URL");
        preserved(); ++cases;
    }
    reset("get:wifi:ota_url", 2);
    check(SaveProvisioningOtaUrl("https://new.example/ota") != ESP_OK, "Second NVS read failure returned success");
    check(stored["wifi"]["ota_url"] == "https://old.example/ota", "Failed read published new URL");
    preserved(); ++cases;
    reset("erase_key:wifi:ota_url");
    check(SaveProvisioningOtaUrl("") != ESP_OK, "OTA erase failure returned success");
    check(stored["wifi"]["ota_url"] == "https://old.example/ota", "Failed erase changed URL");
    preserved(); ++cases;

    for (const auto& invalid : std::vector<std::string>{"ftp://host/ota", "https://", "http:///ota",
            "http://?x", "https://#fragment", "http://has space/ota", "http://host\\path",
            std::string("https://host/") + std::string(256, 'x'), "https://host/\npath"}) {
        reset(); const auto before = stored;
        check(SaveProvisioningOtaUrl(invalid) == ESP_ERR_INVALID_ARG, "Invalid URL accepted");
        check(stored == before && events.empty(), "Invalid URL touched NVS"); ++cases;
    }

    auto parse = [&](const std::string& body, bool valid) {
        auto root = ParseProvisioningJson(body.data(), body.size());
        check(static_cast<bool>(root) == valid, "Wrong strict JSON parsing result"); ++cases;
    };
    parse(R"({"ota_url":"https://host/ota"})", true);
    parse(" \r\n {\"ota_url\":\"\"} \t", true);
    parse(R"({"ota_url":"http://host/ota","remember_bssid":true,"max_tx_power":44})", true);
    parse(R"({"ota_url":"http://host/\\u0000"})", true);
    parse("{}", true);
    parse("", false); parse("[]", false); parse("null", false);
    parse(R"({"ota_url":"one","ota_url":"two"})", false);
    parse(R"({"ota_url":"one"} {"ota_url":"two"})", false);
    parse(R"({"ota_url":"bad\u0000hidden"})", false);
    parse(std::string("{\"ota_url\":\"bad\nurl\"}"), false);
    parse(std::string("{}") + '\0' + "{}", false);
    parse(std::string("{}") + '\v', false);
    parse(R"({"ota_url":"one",})", false);
    parse(std::string(1025, ' '), false);
    std::cout << cases << " provisioning JSON/NVS cases passed\n";
}
'''


def main():
    env = os.environ.copy()
    compiler_bin = host.VC / "bin" / "Hostx64" / "x64"
    env["PATH"] = str(compiler_bin) + os.pathsep + env["PATH"]
    env["INCLUDE"] = os.pathsep.join(str(path) for path in
        (host.VC / "include", host.SDK / "Include" / host.SDK_VERSION / "ucrt",
         host.SDK / "Include" / host.SDK_VERSION / "shared", host.SDK / "Include" / host.SDK_VERSION / "um"))
    env["LIB"] = os.pathsep.join(str(path) for path in
        (host.VC / "lib" / "x64", host.SDK / "Lib" / host.SDK_VERSION / "ucrt" / "x64",
         host.SDK / "Lib" / host.SDK_VERSION / "um" / "x64"))
    with tempfile.TemporaryDirectory(prefix="provisioning-host-", dir=Path(__file__).parent) as temporary:
        directory = Path(temporary)
        (directory / "nvs.h").write_text(NVS_HEADER, encoding="utf-8")
        test = directory / "provisioning_storage_host.cc"
        test.write_text(SOURCE, encoding="utf-8")
        exe = directory / "provisioning_storage_host.exe"
        json_source = host.IDF / "components" / "json" / "cJSON"
        result = subprocess.run([str(compiler_bin / "cl.exe"), "/nologo", "/std:c++17", "/EHsc", "/utf-8",
                                 "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(directory),
                                 "/I" + str(host.COMPONENT / "include"), "/I" + str(json_source),
                                 str(test), str(json_source / "cJSON.c"), "/Fe:" + str(exe),
                                 "/Fo:" + str(directory) + os.sep], env=env, cwd=directory,
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        subprocess.run([str(exe)], cwd=directory, check=True)


if __name__ == "__main__":
    main()
