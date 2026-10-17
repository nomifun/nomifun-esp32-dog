"""Compile exact provisioning HTTP handlers against real cJSON and fake I/O.

Request chunks, interrupted bodies, storage errors, and exit-task scheduling are
controlled locally. No HTTP server, Wi-Fi adapter, task threads or device run.
"""

import os
from pathlib import Path
import subprocess
import tempfile

import test_saved_add as host
import test_provisioning_storage as storage


PRELUDE = r'''
#include <cstdint>
#include <functional>
#include "saved_wifi_request.h"
#define ESP_LOGE(...) ((void)0)
#define TAG "host-test"
const char* esp_err_to_name(esp_err_t) { return "injected error"; }
constexpr int HTTPD_RESP_USE_STRLEN = -1;
constexpr int HTTPD_SOCK_ERR_TIMEOUT = -3;
struct httpd_req_t {
    size_t content_len;
    void* user_ctx;
    std::string body;
    size_t received = 0;
    size_t chunk = 1;
    int timeout_after = -1;
    bool fail_send = false;
    std::string response;
    std::string status = "200 OK";
    std::map<std::string, std::string> response_headers;
};
int httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
int httpd_resp_set_hdr(httpd_req_t* req, const char* key, const char* value) {
    req->response_headers[key] = value; return ESP_OK;
}
const char index_html_start[] = "portal";
const char done_html_start[] = "saved";
int httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = status; return ESP_OK; }
int httpd_resp_send(httpd_req_t* req, const char* data, int length) {
    events.push_back("http:send");
    req->response.assign(data, length < 0 ? std::strlen(data) : length);
    return req->fail_send ? ESP_FAIL : ESP_OK;
}
constexpr int HTTPD_500_INTERNAL_SERVER_ERROR = 500;
int httpd_resp_send_err(httpd_req_t* req, int, const char* message) {
    req->status = "500 Internal Server Error"; return httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
}
int httpd_req_recv(httpd_req_t* req, char* data, size_t maximum) {
    if (req->timeout_after >= 0 && req->received >= static_cast<size_t>(req->timeout_after))
        return HTTPD_SOCK_ERR_TIMEOUT;
    const auto length = std::min({maximum, req->chunk, req->body.size() - req->received});
    std::memcpy(data, req->body.data() + req->received, length);
    req->received += length;
    return static_cast<int>(length);
}
int callbacks = 0;
struct WifiConfigurationAp {
    bool connect_ok = true;
    bool save_ok = true;
    int connects = 0;
    int saves = 0;
    std::string last_ssid;
    std::string last_password;
    std::string ota_url_ = "https://old.example/ota";
    int8_t max_tx_power_ = 80;
    bool remember_bssid_ = false;
    bool sleep_mode_ = false;
    std::function<void()> on_exit_requested_ = [] { ++callbacks; events.push_back("board:exit_callback"); };
    bool exit_pending_ = false;
    std::string connection_failure_;
    bool ConnectToWifi(const std::string& ssid, const std::string& password) {
        ++connects; last_ssid = ssid; last_password = password;
        events.push_back("wifi:connect"); return connect_ok;
    }
    bool Save(const std::string& ssid, const std::string& password) {
        ++saves; last_ssid = ssid; last_password = password;
        events.push_back("wifi:save"); return save_ok;
    }
};
esp_err_t nvs_set_i8(nvs_handle_t handle, const char* key, int8_t value) {
    return nvs_set_str(handle, key, std::to_string(value).c_str());
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value) {
    return nvs_set_str(handle, key, std::to_string(value).c_str());
}
bool power_ok = true;
int power_calls = 0;
esp_err_t esp_wifi_set_max_tx_power(int8_t) { ++power_calls; return power_ok ? ESP_OK : ESP_FAIL; }
struct FakeTask {
    void (*body)(void*) = nullptr;
    void* context = nullptr;
    bool notified = false;
    bool deleted = false;
};
using TaskHandle_t = FakeTask*;
constexpr int pdTRUE = 1;
constexpr int pdPASS = 1;
constexpr int pdFAIL = 0;
unsigned pdMS_TO_TICKS(unsigned ms) { return ms; }
bool create_task_ok = true;
FakeTask task;
FakeTask* running_task = nullptr;
constexpr unsigned portMAX_DELAY = 0xffffffffu;
int xTaskCreate(void (*body)(void*), const char*, unsigned, void* context, unsigned, TaskHandle_t* output) {
    events.push_back("task:create");
    if (!create_task_ok) return pdFAIL;
    task = FakeTask{body, context}; *output = &task; return pdPASS;
}
unsigned ulTaskNotifyTake(int, unsigned) {
    events.push_back("task:wait");
    return running_task && running_task->notified ? 1 : 0;
}
void vTaskDelay(unsigned) { events.push_back("task:delay"); }
void vTaskDelete(TaskHandle_t handle) {
    events.push_back("task:delete");
    if (handle) handle->deleted = true;
    else if (running_task) running_task->deleted = true;
}
void xTaskNotifyGive(TaskHandle_t handle) {
    events.push_back("task:notify");
    check(handle && !handle->deleted, "Notified deleted or null task");
    handle->notified = true;
}
void run_scheduled_task() {
    check(task.body && !task.deleted, "No runnable exit task");
    running_task = &task; task.body(task.context); running_task = nullptr;
}
void reset_http(const std::string& failure = "") {
    reset(failure); power_ok = true; power_calls = 0; create_task_ok = true;
    task = FakeTask{}; running_task = nullptr; callbacks = 0;
}
bool response_success(const httpd_req_t& request) {
    auto json = ParseProvisioningJson(request.response.data(), request.response.size());
    check(static_cast<bool>(json), "HTTP response is not one JSON object");
    auto result = cJSON_GetObjectItemCaseSensitive(json.get(), "success");
    check(cJSON_IsBool(result), "HTTP success is missing or not boolean");
    return cJSON_IsTrue(result);
}
'''

CASES = r'''
int main() {
    const std::string credentials = R"({"ssid":"Home","password":"12345678"})";
    for (size_t chunk : {size_t(1), size_t(7), size_t(1024)}) {
        reset_http(); WifiConfigurationAp ap;
        httpd_req_t req{credentials.size(), &ap, credentials}; req.chunk = chunk;
        check(form_submit_handler(&req) == ESP_OK, "Valid submit returned failure");
        check(response_success(req) && req.status == "200 OK", "Valid submit did not succeed");
        check(req.response.find("\"saved\":true") != std::string::npos && req.response.find("\"restarting\":true") != std::string::npos, "Accepted submit did not describe saved/restarting state");
        check(ap.connects == 0 && ap.saves == 1, "HTTP handler connected Wi-Fi or did not save");
        check(ap.last_ssid == "Home" && ap.last_password == "12345678", "Credentials changed in transit");
        check(req.received == credentials.size(), "Body not fully consumed");
        check(task.notified && !task.deleted && ap.exit_pending_, "Saved credentials did not schedule restart");
        check(event_index("task:create") < event_index("wifi:save"), "Worker was not prepared before save");
        check(event_index("wifi:save") < event_index("http:send") && event_index("http:send") < event_index("task:notify"), "Save/response/notify ordering wrong");
        check(callbacks == 0, "Restart ran inside HTTP handler");
        run_scheduled_task(); check(callbacks == 1 && task.deleted, "Saved request did not restart independently"); ++cases;
    }

    auto invalid_submit = [&](const std::string& body, size_t extra = 0, int timeout = -1) {
        reset_http(); WifiConfigurationAp ap;
        httpd_req_t req{body.size() + extra, &ap, body}; req.timeout_after = timeout;
        form_submit_handler(&req);
        check(!response_success(req), "Invalid Wi-Fi submit succeeded");
        check(req.status == (timeout >= 0 ? "408 Request Timeout" : "400 Bad Request"), "Wrong invalid-submit status");
        check(ap.connects == 0 && ap.saves == 0, "Invalid submit touched Wi-Fi");
        check(std::none_of(events.begin(), events.end(), [](const std::string& e) { return e.rfind("open:", 0) == 0; }), "Invalid submit opened NVS"); ++cases;
    };
    invalid_submit(""); invalid_submit(std::string(1025, 'x'));
    invalid_submit(credentials, 4); invalid_submit(credentials, 0, 3);
    invalid_submit("{}"); invalid_submit("[]"); invalid_submit("null");
    invalid_submit(R"({"ssid":"Home","password":null})");
    invalid_submit(R"({"ssid":"Home","ssid":"Other","password":""})");
    invalid_submit(R"({"ssid":"Home","password":""} {})");
    invalid_submit(R"({"ssid":"Home","password":"bad\u0000secret"})");

    reset_http(); WifiConfigurationAp failed_save; failed_save.save_ok = false;
    httpd_req_t save_req{credentials.size(), &failed_save, credentials};
    form_submit_handler(&save_req);
    check(save_req.status == "500 Internal Server Error" && !response_success(save_req), "Wi-Fi save failure returned success");
    check(failed_save.connects == 0 && failed_save.saves == 1, "Save failure call count wrong");
    check(task.deleted && !task.notified && !failed_save.exit_pending_ && callbacks == 0, "Save failure leaked or ran restart task"); ++cases;

    reset_http(); create_task_ok = false; WifiConfigurationAp submit_no_task;
    httpd_req_t no_task_req{credentials.size(), &submit_no_task, credentials};
    form_submit_handler(&no_task_req);
    check(no_task_req.status == "503 Service Unavailable" && !response_success(no_task_req), "Submit task failure returned success");
    check(submit_no_task.saves == 0 && submit_no_task.connects == 0 && callbacks == 0, "Unrestartable submit saved credentials"); ++cases;

    reset_http(); WifiConfigurationAp submit_no_callback; submit_no_callback.on_exit_requested_ = {};
    httpd_req_t no_callback_req{credentials.size(), &submit_no_callback, credentials};
    form_submit_handler(&no_callback_req);
    check(no_callback_req.status == "503 Service Unavailable" && !response_success(no_callback_req), "Submit missing callback returned success");
    check(submit_no_callback.saves == 0 && !task.body, "Submit missing callback changed state"); ++cases;

    reset_http(); WifiConfigurationAp response_lost;
    httpd_req_t lost_req{credentials.size(), &response_lost, credentials}; lost_req.fail_send = true;
    check(form_submit_handler(&lost_req) == ESP_FAIL, "Submit response loss returned success");
    check(response_lost.saves == 1 && task.notified && !task.deleted, "Response loss stranded durable configuration");
    run_scheduled_task(); check(callbacks == 1 && task.deleted, "Response loss did not restart"); ++cases;

    reset_http(); WifiConfigurationAp repeated;
    httpd_req_t first_req{credentials.size(), &repeated, credentials}; form_submit_handler(&first_req);
    httpd_req_t repeated_req{credentials.size(), &repeated, credentials}; form_submit_handler(&repeated_req);
    check(repeated_req.status == "409 Conflict" && !response_success(repeated_req), "Pending restart accepted duplicate submit");
    check(repeated.saves == 1 && task.notified && !task.deleted, "Duplicate submit changed saved credentials or lost worker");
    run_scheduled_task(); check(callbacks == 1 && task.deleted, "Duplicate submit changed restart count"); ++cases;

    const std::string configuration = R"({"ota_url":"https://new.example/ota","max_tx_power":44,"remember_bssid":true,"sleep_mode":true})";
    reset_http(); WifiConfigurationAp configured;
    httpd_req_t config_req{configuration.size(), &configured, configuration};
    advanced_submit_handler(&config_req);
    check(response_success(config_req) && config_req.status == "200 OK", "Chunked advanced request failed");
    check(config_req.received == configuration.size(), "Advanced body not fully consumed");
    check(configured.ota_url_ == "https://new.example/ota" && configured.max_tx_power_ == 44 &&
          configured.remember_bssid_ && configured.sleep_mode_, "Advanced object state not updated");
    check(stored["wifi"]["ota_url"] == configured.ota_url_ && stored["wifi"]["max_tx_power"] == "44", "Advanced settings not committed");
    check(stored["websocket"].empty() && stored["mqtt"].empty(), "Advanced save retained old credentials");
    check(power_calls == 1 && handles.empty(), "Advanced power call or handle cleanup wrong"); ++cases;

    auto invalid_advanced = [&](const std::string& body, size_t extra = 0, int timeout = -1) {
        reset_http(); const auto previous = stored; WifiConfigurationAp ap;
        httpd_req_t req{body.size() + extra, &ap, body}; req.timeout_after = timeout;
        advanced_submit_handler(&req);
        check(!response_success(req), "Invalid advanced configuration succeeded");
        check(req.status == (timeout >= 0 ? "408 Request Timeout" : "400 Bad Request"), "Wrong advanced validation status");
        check(stored == previous && handles.empty() && power_calls == 0, "Invalid advanced request touched storage or Wi-Fi power");
        check(ap.connects == 0 && ap.saves == 0 && ap.ota_url_ == "https://old.example/ota", "Invalid advanced request changed AP state");
        check(std::none_of(events.begin(), events.end(), [](const std::string& e) { return e.rfind("open:", 0) == 0; }), "Validation performed NVS I/O"); ++cases;
    };
    invalid_advanced(""); invalid_advanced(std::string(1025, 'x'));
    invalid_advanced(configuration, 3); invalid_advanced(configuration, 0, 7);
    invalid_advanced("[]"); invalid_advanced("null"); invalid_advanced("{broken}");
    invalid_advanced(R"({"unknown":true})");
    invalid_advanced(R"({"ota_url":"https://new.example/ota","unknown":true})");
    invalid_advanced(R"({"ota_url":"https://new.example/ota","ota_url":"https://other.example/ota"})");
    invalid_advanced(R"({"ota_url":false})"); invalid_advanced(R"({"ota_url":"ftp://host/ota"})");
    invalid_advanced(R"({"max_tx_power":"44"})"); invalid_advanced(R"({"max_tx_power":44.5})");
    invalid_advanced(R"({"max_tx_power":7})"); invalid_advanced(R"({"max_tx_power":81})");
    invalid_advanced(R"({"remember_bssid":1})"); invalid_advanced(R"({"sleep_mode":"true"})");

    for (const char* failure : {"open:wifi", "commit:websocket", "commit:mqtt", "set:wifi:ota_url", "commit:wifi"}) {
        reset_http(failure); WifiConfigurationAp ap;
        const std::string body = R"({"ota_url":"https://new.example/ota"})";
        httpd_req_t req{body.size(), &ap, body}; advanced_submit_handler(&req);
        check(req.status == "500 Internal Server Error" && !response_success(req), "OTA storage failure succeeded");
        check(ap.ota_url_ == "https://old.example/ota" && stored["wifi"]["ota_url"] == "https://old.example/ota", "Failed OTA changed published URL");
        check(handles.empty(), "Failed OTA HTTP request leaked handle"); ++cases;
    }

    reset_http(); WifiConfigurationAp no_callback; no_callback.on_exit_requested_ = {};
    httpd_req_t absent_req{0, &no_callback, ""}; exit_config_handler(&absent_req);
    check(absent_req.status == "503 Service Unavailable" && !response_success(absent_req), "Missing callback did not fail");
    check(!task.body && callbacks == 0, "Missing callback scheduled exit"); ++cases;

    auto configure_callback = [](WifiConfigurationAp& ap) {
        ap.on_exit_requested_ = [] { ++callbacks; events.push_back("board:exit_callback"); };
    };
    reset_http(); create_task_ok = false; WifiConfigurationAp no_task; configure_callback(no_task);
    httpd_req_t task_req{0, &no_task, ""}; exit_config_handler(&task_req);
    check(task_req.status == "503 Service Unavailable" && !response_success(task_req), "Task creation failure did not fail");
    check(callbacks == 0 && !task.body, "Failed task creation ran callback"); ++cases;

    reset_http(); WifiConfigurationAp send_failed; configure_callback(send_failed);
    httpd_req_t send_req{0, &send_failed, ""}; send_req.fail_send = true;
    check(exit_config_handler(&send_req) == ESP_FAIL, "HTTP send failure returned success");
    check(task.deleted && !task.notified && callbacks == 0, "Failed HTTP send notified or ran callback");
    check(event_index("http:send") < event_index("task:delete"), "Task deleted before HTTP result"); ++cases;

    reset_http(); WifiConfigurationAp exit_ok; configure_callback(exit_ok);
    httpd_req_t exit_req{0, &exit_ok, ""}; check(exit_config_handler(&exit_req) == ESP_OK, "Exit handler failed");
    check(response_success(exit_req) && task.notified && !task.deleted, "Successful exit did not schedule notify");
    check(callbacks == 0, "Exit handler ran callback synchronously");
    check(event_index("http:send") < event_index("task:notify"), "Task notified before response send");
    httpd_req_t exit_repeat_req{0, &exit_ok, ""}; exit_config_handler(&exit_repeat_req);
    check(response_success(exit_repeat_req) && task.notified && !task.deleted, "Duplicate exit interfered with worker");
    run_scheduled_task();
    check(callbacks == 1 && task.deleted, "Scheduled task did not call board exit once and delete itself");
    check(event_index("task:notify") < event_index("board:exit_callback"), "Callback ran before task notification"); ++cases;

    reset_http(); WifiConfigurationAp failure_reason;
    failure_reason.connection_failure_ = "Authentication failed: \"reason\"\nRetry";
    httpd_req_t reason_req{0, &failure_reason, ""}; advanced_config_handler(&reason_req);
    auto reason_json = ParseProvisioningJson(reason_req.response.data(), reason_req.response.size());
    check(static_cast<bool>(reason_json), "Connection failure JSON invalid");
    auto reason_item = cJSON_GetObjectItemCaseSensitive(reason_json.get(), "connection_failure");
    check(cJSON_IsString(reason_item) && reason_item->valuestring == failure_reason.connection_failure_, "Connection reason changed or not escaped");
    check(!cJSON_GetObjectItemCaseSensitive(reason_json.get(), "password"), "Configuration read exposed Wi-Fi password");
    check(reason_req.response_headers["Cache-Control"] == "no-store", "Configuration GET allowed stale cache"); ++cases;
    for (const auto& handler : {+index_html_handler, +done_html_handler}) {
        reset_http(); httpd_req_t html_req{0, nullptr, ""}; handler(&html_req);
        check(html_req.response_headers["Cache-Control"] == "no-store", "Portal HTML allowed stale cache"); ++cases;
    }

    std::cout << cases << " provisioning HTTP/body/exit-task cases passed\n";
}
'''


def extract_handler(source, name):
    start = source.index(".handler = ", source.index("httpd_uri_t " + name)) + len(".handler = ")
    end = source.index(",\n        .user_ctx = ", start)
    return "auto " + name + "_handler = " + source[start:end] + ";\n"


def main():
    source = (host.COMPONENT / "wifi_configuration_ap.cc").read_text(encoding="utf-8")
    helpers_start = source.index("static bool ReadProvisioningBody(")
    helpers_end = source.index("WifiConfigurationAp::WifiConfigurationAp()", helpers_start)
    body = storage.SOURCE[:storage.SOURCE.index("int main() {")]
    body += PRELUDE + source[helpers_start:helpers_end]
    body += "\n".join(extract_handler(source, name) for name in ("form_submit", "advanced_submit", "exit_config", "advanced_config", "index_html", "done_html"))
    body += CASES
    env = os.environ.copy()
    compiler_bin = host.VC / "bin" / "Hostx64" / "x64"
    env["PATH"] = str(compiler_bin) + os.pathsep + env["PATH"]
    env["INCLUDE"] = os.pathsep.join(str(path) for path in
        (host.VC / "include", host.SDK / "Include" / host.SDK_VERSION / "ucrt",
         host.SDK / "Include" / host.SDK_VERSION / "shared", host.SDK / "Include" / host.SDK_VERSION / "um"))
    env["LIB"] = os.pathsep.join(str(path) for path in
        (host.VC / "lib" / "x64", host.SDK / "Lib" / host.SDK_VERSION / "ucrt" / "x64",
         host.SDK / "Lib" / host.SDK_VERSION / "um" / "x64"))
    with tempfile.TemporaryDirectory(prefix="provisioning-http-", dir=Path(__file__).parent) as temporary:
        directory = Path(temporary)
        (directory / "nvs.h").write_text(storage.NVS_HEADER, encoding="utf-8")
        test = directory / "provisioning_http_host.cc"
        test.write_text(body, encoding="utf-8")
        exe = directory / "provisioning_http_host.exe"
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
