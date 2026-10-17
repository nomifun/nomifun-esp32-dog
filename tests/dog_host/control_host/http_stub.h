#pragma once
#include <map>
#include <string>
using esp_err_t = int;
using httpd_handle_t = void*;
enum httpd_method_t { HTTP_GET, HTTP_POST, HTTP_DELETE, HTTP_HEAD, HTTP_OPTIONS };
constexpr int ESP_OK = 0, ESP_FAIL = -1, HTTPD_SOCK_ERR_TIMEOUT = -3;
struct httpd_req_t {
    httpd_method_t method = HTTP_GET;
    std::string query, body, response, status = "200 OK", uri;
    size_t content_len = 0, received = 0;
    std::map<std::string, std::string> headers;
};
