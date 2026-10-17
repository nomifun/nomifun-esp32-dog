#include <cassert>
#include <charconv>
#include <cstring>
#include <iostream>
#include <vector>
#include "esp_http_server.h"
#define ESP_LOGI(...) ((void)0)
constexpr int HTTPD_RESP_USE_STRLEN = -1;
int httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = status; return ESP_OK; }
int httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
int httpd_resp_set_hdr(httpd_req_t* req, const char* key, const char* value) { req->headers[key] = value; return ESP_OK; }
int httpd_resp_send(httpd_req_t* req, const char* value, int length) { req->response.assign(value, length < 0 ? std::strlen(value) : length); return ESP_OK; }
class SsidManager {
public:
    int deletes = 0;
    bool save_ok = true;
    static SsidManager& GetInstance() { static SsidManager manager; return manager; }
    std::vector<int> GetSsidList() const { return {1, 2}; }
    bool RemoveSsid(int index) { assert(index >= 0 && index < 2); ++deletes; return save_ok; }
};

// INSERT_PRODUCTION_FUNCTIONS

int main() {
    int cases = 0;
    auto& manager = SsidManager::GetInstance();
    for (const char* uri : {"/saved/delete", "/saved/delete?index=", "/saved/delete?index=-1", "/saved/delete?index=2",
                            "/saved/delete?index=0.5", "/saved/delete?index=0tail", "/saved/delete?index=9999999999999999999",
                            "/saved/delete?index=+1", "/saved/delete?index= 1"}) {
        httpd_req_t req; req.uri = uri;
        handler(&req); assert(req.status == "400 Bad Request" && manager.deletes == 0); ++cases;
    }
    for (bool saved : {false, true}) {
        manager.save_ok = saved;
        httpd_req_t req; req.uri = "/saved/delete?index=1";
        handler(&req); assert(req.status == (saved ? "200 OK" : "500 Internal Server Error"));
        assert(req.headers["Connection"] == "close");
        assert(req.response == (saved ? "{}" : "{\"success\":false,\"error\":\"Failed to delete saved network\"}")); ++cases;
    }
    assert(manager.deletes == 2);
    std::cout << cases << " provisioning delete-index/persistence HTTP cases passed\n";
}
