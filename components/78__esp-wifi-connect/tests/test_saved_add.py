"""Run the exact saved_add lambda with real cJSON and fake HTTP/NVS, offline.

Uses the installed MSVC Build Tools, not ESP-IDF/CMake or the device. Compiler
outputs are confined to a temporary directory below this tests directory.
"""

import os
from pathlib import Path
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]
if not os.environ.get("IDF_PATH"):
    raise SystemExit("IDF_PATH must point at the installed ESP-IDF checkout (for components/json/cJSON)")
IDF = Path(os.environ["IDF_PATH"])
VC = sorted(Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC").iterdir())[-1]
SDK = Path("C:/Program Files (x86)/Windows Kits/10")
SDK_VERSION = sorted((SDK / "Lib").iterdir())[-1].name

PRELUDE = r'''
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include "saved_wifi_request.h"
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int HTTPD_RESP_USE_STRLEN = -1;
constexpr int HTTPD_SOCK_ERR_TIMEOUT = -3;
struct WifiConfigurationAp {
    int saves = 0;
    bool save_succeeds = true;
    std::string ssid;
    std::string password;
    bool Save(const std::string& name, const std::string& secret) {
        ++saves;
        ssid = name;
        password = secret;
        return save_succeeds;
    }
};
struct httpd_req_t {
    size_t content_len;
    void* user_ctx;
    std::string body;
    size_t received = 0;
    size_t chunk = 1;
    bool timeout = false;
    std::string response;
    std::string status = "200 OK";
};
int httpd_resp_set_type(httpd_req_t*, const char*) { return 0; }
int httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return 0; }
int httpd_resp_set_status(httpd_req_t* req, const char* status) {
    req->status = status;
    return 0;
}
int httpd_resp_send(httpd_req_t* req, const char* data, int length) {
    req->response.assign(data, length < 0 ? std::strlen(data) : length);
    return 0;
}
int httpd_req_recv(httpd_req_t* req, char* data, size_t maximum) {
    if (req->timeout && req->received != 0) return HTTPD_SOCK_ERR_TIMEOUT;
    const size_t length = std::min({maximum, req->chunk, req->body.size() - req->received});
    std::memcpy(data, req->body.data() + req->received, length);
    req->received += length;
    return static_cast<int>(length);
}
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    auto handler =
'''

CASES = r''';
    int cases = 0;
    auto attempt = [&](const std::string& body, bool valid, size_t chunk = 1,
                       size_t additional_length = 0, bool timeout = false) {
        WifiConfigurationAp ap;
        httpd_req_t req{body.size() + additional_length, &ap, body};
        req.chunk = chunk;
        req.timeout = timeout;
        handler(&req);
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> response(cJSON_Parse(req.response.c_str()), cJSON_Delete);
        check(cJSON_IsObject(response.get()), "Response is not JSON");
        const auto success = cJSON_GetObjectItemCaseSensitive(response.get(), "success");
        check(valid ? cJSON_IsTrue(success) : cJSON_IsFalse(success), "Wrong success response");
        check(ap.saves == (valid ? 1 : 0), "Invalid request persisted or valid request not saved");
        check(valid ? req.status == "200 OK" : req.status != "200 OK", "Wrong HTTP status");
        ++cases;
    };
    attempt(R"({"ssid":"Home","password":"12345678"})", true);
    attempt(R"({"ssid":"Open network","password":""})", true, 7);
    attempt(R"({"password":"secret","ssid":"Home"})", true, 100);
    attempt(" {\"ssid\":\"Home\",\"password\":\"secret\"} \r\n", true);
    attempt(R"({"ssid":"\u4f60\u597d","password":"a\"b\\c"})", true);
    attempt(R"({"ssid":"literal\\u0000","password":""})", true);
    attempt(std::string("{\"ssid\":\"") + std::string(32, 'x') + "\",\"password\":\"" + std::string(64, 'a') + "\"}", true);
    attempt(std::string("{\"ssid\":\"") + std::string(33, 'x') + "\",\"password\":\"\"}", false);
    attempt(std::string("{\"ssid\":\"Home\",\"password\":\"") + std::string(65, 'a') + "\"}", false);
    attempt("", false);
    attempt("{}", false);
    attempt("[]", false);
    attempt("null", false);
    attempt(R"({"ssid":"","password":""})", false);
    attempt(R"({"ssid":"   ","password":""})", false);
    attempt(R"({"ssid":123,"password":""})", false);
    attempt(R"({"ssid":"Home","password":null})", false);
    attempt(R"({"ssid":"Home"})", false);
    attempt(R"({"password":""})", false);
    attempt(R"({"SSID":"Home","password":""})", false);
    attempt(R"({"ssid":"Home","ssid":"Other","password":""})", false);
    attempt(R"({"ssid":"Home","password":"","password":"secret"})", false);
    attempt(R"({"ssid":"Home","password":"","extra":true})", false);
    attempt(R"({"ssid":"Home","password":""} {})", false);
    attempt(R"({"ssid":"Home","password":"",})", false);
    attempt(R"({"ssid":"Home","password":"\u0000secret"})", false);
    attempt(R"({"ssid":"Home\u0000hidden","password":""})", false);
    attempt(std::string("{\"ssid\":\"bad\nname\",\"password\":\"\"}"), false);
    attempt(std::string("{\"ssid\":\"Home\",\"password\":\"\"}\v"), false);
    attempt(std::string("{\"ssid\":\"Home\",\"password\":\"\"}") + '\0' + "{}", false);
    attempt(std::string(1025, 'x'), false);
    attempt(R"({"ssid":"Home","password":""})", false, 3, 5);
    attempt(R"({"ssid":"Home","password":""})", false, 3, 0, true);
    {
        WifiConfigurationAp ap;
        ap.save_succeeds = false;
        const std::string body = R"({"ssid":"Home","password":"secret"})";
        httpd_req_t req{body.size(), &ap, body};
        handler(&req);
        check(req.status == "500 Internal Server Error", "Failed persistence returned success status");
        check(req.response.find("\"success\":false") != std::string::npos, "Failed persistence returned success body");
        ++cases;
    }
    std::cout << cases << " Save-only HTTP/JSON cases passed\n";
}
'''


def main():
    source = (COMPONENT / "wifi_configuration_ap.cc").read_text(encoding="utf-8")
    start = source.index(".handler = ", source.index("httpd_uri_t saved_add")) + len(".handler = ")
    end = source.index(",\n        .user_ctx = this", start)
    handler = source[start:end]
    assert "ConnectToWifi" not in handler and "esp_restart" not in handler
    assert source.count('config.max_uri_handlers = 25;') == 1
    assert source.count('httpd_register_uri_handler(server_, &saved_add)') == 1
    env = os.environ.copy()
    compiler_bin = VC / "bin" / "Hostx64" / "x64"
    env["PATH"] = str(compiler_bin) + os.pathsep + env["PATH"]
    env["INCLUDE"] = os.pathsep.join(str(path) for path in
        (VC / "include", SDK / "Include" / SDK_VERSION / "ucrt",
         SDK / "Include" / SDK_VERSION / "shared", SDK / "Include" / SDK_VERSION / "um"))
    env["LIB"] = os.pathsep.join(str(path) for path in
        (VC / "lib" / "x64", SDK / "Lib" / SDK_VERSION / "ucrt" / "x64",
         SDK / "Lib" / SDK_VERSION / "um" / "x64"))
    with tempfile.TemporaryDirectory(prefix="host-test-", dir=Path(__file__).parent) as temporary:
        directory = Path(temporary)
        test = directory / "saved_add_host.cc"
        test.write_text(PRELUDE + handler + CASES, encoding="utf-8")
        exe = directory / "saved_add_host.exe"
        json_source = IDF / "components" / "json" / "cJSON"
        result = subprocess.run([str(compiler_bin / "cl.exe"), "/nologo", "/std:c++17", "/EHsc", "/utf-8",
                                 "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(COMPONENT / "include"),
                                 "/I" + str(json_source), str(test), str(json_source / "cJSON.c"),
                                 "/Fe:" + str(exe), "/Fo:" + str(directory) + os.sep],
                                env=env, cwd=directory, capture_output=True, text=True,
                                encoding="utf-8", errors="replace")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        subprocess.run([str(exe)], cwd=directory, check=True)


if __name__ == "__main__":
    main()
