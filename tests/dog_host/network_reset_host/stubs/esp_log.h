#pragma once
template<class... Args> inline void HostLog(Args&&...) {}
#define ESP_LOGE(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
