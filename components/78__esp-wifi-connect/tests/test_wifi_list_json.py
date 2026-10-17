"""Compile the exact scan/saved-list handlers with real cJSON, offline.

Run with Windows MSVC only. Fixed-width scan fields, SSID byte round trips,
allocation failures and HTTP send errors are injected without starting a
network, touching a Wi-Fi adapter, using WSL or connecting to the robot.
"""

import os
from pathlib import Path
import subprocess
import tempfile

import test_saved_add as host


PRELUDE = r'''
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <cJSON.h>
#include "wifi_credentials.h"
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int HTTPD_RESP_USE_STRLEN = -1;
int cases = 0;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int allocation = 0;
int fail_at = 0;
std::set<void*> live;
void* allocate(size_t size) {
    if (++allocation == fail_at) return nullptr;
    void* result = std::malloc(size);
    if (result) live.insert(result);
    return result;
}
void deallocate(void* pointer) {
    if (!pointer) return;
    check(live.erase(pointer) == 1, "Unknown allocation/double free");
    std::free(pointer);
}
void reset_allocations(int fail = 0) {
    check(live.empty(), "Previous request leaked cJSON allocations");
    allocation = 0; fail_at = fail;
}
struct wifi_ap_record_t {
    uint8_t ssid[33] = {};
    int8_t rssi = 0;
    int authmode = 0;
};
struct WifiConfigurationAp {
    std::mutex mutex_;
    std::vector<wifi_ap_record_t> ap_records_;
    std::vector<wifi_ap_record_t> GetAccessPoints();
};
struct SsidItem { std::string ssid; std::string password; };
struct SsidManager {
    std::vector<SsidItem> records;
    static SsidManager& GetInstance() { static SsidManager value; return value; }
    const std::vector<SsidItem>& GetSsidList() { return records; }
};
struct httpd_req_t {
    void* user_ctx = nullptr;
    std::string response;
    std::string status = "200 OK";
    std::string type;
    std::map<std::string, std::string> headers;
    int sends = 0;
    bool fail_send = false;
};
int httpd_resp_set_type(httpd_req_t* req, const char* value) { req->type = value; return ESP_OK; }
int httpd_resp_set_hdr(httpd_req_t* req, const char* key, const char* value) {
    req->headers[key] = value; return ESP_OK;
}
int httpd_resp_set_status(httpd_req_t* req, const char* value) { req->status = value; return ESP_OK; }
int httpd_resp_send(httpd_req_t* req, const char* data, int length) {
    if (req->user_ctx) {
        auto* ap = static_cast<WifiConfigurationAp*>(req->user_ctx);
        check(ap->mutex_.try_lock(), "Scan mutex held during network I/O");
        ap->mutex_.unlock();
    }
    ++req->sends;
    req->response.assign(data, length < 0 ? std::strlen(data) : length);
    return req->fail_send ? ESP_FAIL : ESP_OK;
}
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json parse(const httpd_req_t& req) {
    fail_at = 0;
    const char* end = nullptr;
    Json json(cJSON_ParseWithLengthOpts(req.response.c_str(), req.response.size() + 1, &end, true), cJSON_Delete);
    check(json && end == req.response.c_str() + req.response.size(), "Response is not complete JSON");
    check(req.type == "application/json" && req.headers.at("Connection") == "close" &&
        req.headers.at("Cache-Control") == "no-store", "Missing list response headers");
    check(req.sends == 1, "Response was sent partially or more than once");
    return json;
}
'''


CASES = r'''
int main() {
    cJSON_Hooks hooks{allocate, deallocate}; cJSON_InitHooks(&hooks);
    WifiConfigurationAp ap;
    using Handler = esp_err_t (*)(httpd_req_t*);
    const Handler handlers[] = {+saved_list_handler, +scan_handler};
    auto validate = [&](const httpd_req_t& req, bool scan, const std::vector<std::string>& expected) {
        check(live.empty(), "Handler leaked JSON tree or printed text");
        auto json = parse(req);
        const auto list = scan ? cJSON_GetObjectItemCaseSensitive(json.get(), "aps") : json.get();
        check(req.status == "200 OK" && cJSON_IsArray(list), "List response failed");
        check(cJSON_GetArraySize(list) == expected.size(), "List size changed");
        if (scan) {
            const auto support = cJSON_GetObjectItemCaseSensitive(json.get(), "support_5g");
#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
            check(cJSON_IsTrue(support), "Supported 5G was not advertised");
#else
            check(cJSON_IsFalse(support), "Unsupported 5G was advertised");
#endif
        }
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto item = cJSON_GetArrayItem(list, static_cast<int>(i));
            const auto ssid = scan ? cJSON_GetObjectItemCaseSensitive(item, "ssid") : item;
            check(cJSON_IsString(ssid) && std::string(ssid->valuestring) == expected[i], "SSID bytes changed in JSON round trip");
            if (scan) {
                check(cJSON_GetObjectItemCaseSensitive(item, "rssi")->valueint == -42 - static_cast<int>(i), "RSSI changed");
                check(cJSON_GetObjectItemCaseSensitive(item, "authmode")->valueint == static_cast<int>(i) % 7, "Authmode changed");
            }
        }
    };
    for (int type = 0; type < 2; ++type) {
        reset_allocations(); httpd_req_t req; req.user_ctx = type ? &ap : nullptr;
        check(handlers[type](&req) == ESP_OK, "Empty list failed");
        validate(req, type == 1, {}); ++cases;
    }
    const std::vector<std::string> names{
        "Home", "quotes\"slash\\", "line\ncarriage\rtab\tback\bform\f\x01",
        u8"中文 Wi-Fi", std::string(32, 'X'), u8"测试网络" + std::string(20, 'z'),
        "with trailing space "
    };
    for (size_t i = 0; i < names.size(); ++i) {
        check(names[i].size() <= 32, "Invalid test SSID length");
        SsidManager::GetInstance().records.push_back({names[i], "not-returned-secret"});
        wifi_ap_record_t record;
        std::memcpy(record.ssid, names[i].data(), names[i].size());
        if (names[i].size() == 32) record.ssid[32] = 0xff; // A sentinel must never be read as SSID.
        record.rssi = -42 - static_cast<int>(i); record.authmode = static_cast<int>(i) % 7;
        ap.ap_records_.push_back(record);
    }
    for (int type = 0; type < 2; ++type) {
        reset_allocations(); httpd_req_t req; req.user_ctx = type ? &ap : nullptr;
        check(handlers[type](&req) == ESP_OK, "Special SSID list failed");
        const int total_allocations = allocation;
        validate(req, type == 1, names);
        check(req.response.find("not-returned-secret") == std::string::npos, "Saved password exposed"); ++cases;
        check(live.empty(), "Response parse leaked");
        // Fail every allocation made by the successful handler, including print buffers.
        for (int failure = 1; failure <= total_allocations; ++failure) {
            reset_allocations(failure); httpd_req_t failed; failed.user_ctx = type ? &ap : nullptr;
            check(handlers[type](&failed) == ESP_OK, "Error response send failed unexpectedly");
            check(live.empty(), "Allocation failure leaked partial JSON or print buffer");
            check(failed.status == "500 Internal Server Error", "Allocation failure returned success status");
            {
                auto json = parse(failed);
                check(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json.get(), "success")), "Allocation failure returned success JSON");
            }
            check(live.empty(), "Error response parse leaked"); ++cases;
        }
        reset_allocations(); httpd_req_t failed_send; failed_send.user_ctx = type ? &ap : nullptr; failed_send.fail_send = true;
        check(handlers[type](&failed_send) == ESP_FAIL, "Send failure was falsely reported as success");
        check(live.empty(), "Send failure leaked JSON or print buffer"); ++cases;
    }
    // An AP hidden from broadcast still has a valid empty SSID in the scan schema.
    ap.ap_records_.clear(); ap.ap_records_.push_back(wifi_ap_record_t{});
    ap.ap_records_[0].rssi = -42;
    reset_allocations(); httpd_req_t hidden; hidden.user_ctx = &ap; scan_handler(&hidden);
    validate(hidden, true, {""}); ++cases;
    check(live.empty(), "Final JSON parse leaked");
    cJSON_InitHooks(nullptr);
    std::cout << cases << " Wi-Fi list JSON/SSID/allocation/HTTP cases passed\n";
}
'''


def extract_handler(source, name):
    start = source.index(".handler = ", source.index("httpd_uri_t " + name)) + len(".handler = ")
    end = source.index(",\n        .user_ctx = ", start)
    return "auto " + name + "_handler = " + source[start:end] + ";\n"


def main():
    source = (host.COMPONENT / "wifi_configuration_ap.cc").read_text(encoding="utf-8")
    helper_start = source.index("static esp_err_t SendWifiListJson(")
    helper_end = source.index("static bool ReadProvisioningBody(", helper_start)
    snapshot_start = source.index("std::vector<wifi_ap_record_t> WifiConfigurationAp::GetAccessPoints()")
    snapshot_end = source.index("WifiConfigurationAp::~WifiConfigurationAp()", snapshot_start)
    body = PRELUDE + source[helper_start:helper_end] + source[snapshot_start:snapshot_end]
    body += extract_handler(source, "saved_list") + extract_handler(source, "scan") + CASES
    env = os.environ.copy()
    compiler_bin = host.VC / "bin" / "Hostx64" / "x64"
    env["PATH"] = str(compiler_bin) + os.pathsep + env["PATH"]
    env["INCLUDE"] = os.pathsep.join(str(path) for path in
        (host.VC / "include", host.SDK / "Include" / host.SDK_VERSION / "ucrt",
         host.SDK / "Include" / host.SDK_VERSION / "shared", host.SDK / "Include" / host.SDK_VERSION / "um"))
    env["LIB"] = os.pathsep.join(str(path) for path in
        (host.VC / "lib" / "x64", host.SDK / "Lib" / host.SDK_VERSION / "ucrt" / "x64",
         host.SDK / "Lib" / host.SDK_VERSION / "um" / "x64"))
    with tempfile.TemporaryDirectory(prefix="wifi-list-host-", dir=Path(__file__).parent) as temporary:
        directory = Path(temporary)
        test = directory / "wifi_list_host.cc"
        test.write_text(body, encoding="utf-8")
        json_source = host.IDF / "components" / "json" / "cJSON"
        for support_5g in (False, True):
            exe = directory / ("wifi_list_5g_host.exe" if support_5g else "wifi_list_2g_host.exe")
            command = [str(compiler_bin / "cl.exe"), "/nologo", "/std:c++17", "/EHsc", "/utf-8",
                       "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(host.COMPONENT / "include"),
                       "/I" + str(json_source), str(test), str(json_source / "cJSON.c"),
                       "/Fe:" + str(exe), "/Fo:" + str(directory) + os.sep]
            if support_5g:
                command.insert(6, "/DCONFIG_SOC_WIFI_SUPPORT_5G=1")
            result = subprocess.run(command, env=env, cwd=directory, capture_output=True,
                                    text=True, encoding="utf-8", errors="replace")
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            subprocess.run([str(exe)], cwd=directory, check=True)


if __name__ == "__main__":
    main()
