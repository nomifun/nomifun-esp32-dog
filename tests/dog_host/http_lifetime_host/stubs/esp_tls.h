#pragma once
#include <cstddef>
using esp_err_t=int;
#define ESP_OK 0
#define ESP_TLS_ERR_SSL_WANT_READ -2
#define ESP_TLS_ERR_SSL_WANT_WRITE -3
struct esp_tls { int fd; }; using esp_tls_t=esp_tls;
using esp_tls_error_handle_t=void*;
struct esp_tls_cfg_t { void* crt_bundle_attach; };
esp_tls_t* esp_tls_init();
int esp_tls_conn_new_sync(const char*,size_t,int,const esp_tls_cfg_t*,esp_tls_t*);
int esp_tls_get_error_handle(esp_tls_t*,esp_tls_error_handle_t*);
int esp_tls_get_and_clear_last_error(esp_tls_error_handle_t,int*,int*);
int esp_tls_get_conn_sockfd(esp_tls_t*,int*);
void esp_tls_conn_destroy(esp_tls_t*);
int esp_tls_conn_write(esp_tls_t*,const void*,size_t);
int esp_tls_conn_read(esp_tls_t*,void*,size_t);
