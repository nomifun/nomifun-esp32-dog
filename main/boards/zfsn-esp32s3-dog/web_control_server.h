#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <esp_http_server.h>

class PetDog;
class WS2812LED;

// LAN HTTP control service plus a local firmware-update page. The owner
// starts the service after IP_EVENT_STA_GOT_IP.
// Missing hardware callbacks return an error; they never acknowledge an action.
class WebControlServer {
public:
    struct Callbacks {
        std::function<bool(const std::string&)> set_emotion;
        std::function<bool()> random_emotion;
        std::function<bool(const std::string&)> load_custom_emotion;
        std::function<bool()> sd_mounted;
        std::function<bool()> wake;
        std::function<bool()> sleep;
        std::function<int()> get_volume;
        std::function<bool(int)> set_volume;
        // Reload display/music settings from the native "display" namespace.
        std::function<bool()> settings_changed;
        std::function<bool(const std::string&, bool)> play_music;
    };

    WebControlServer(PetDog& dog, WS2812LED& led, Callbacks callbacks);
    ~WebControlServer();
    esp_err_t Start();
    void Stop();

private:
    PetDog& dog_;
    WS2812LED& led_;
    Callbacks callbacks_;
    httpd_handle_t server_ = nullptr;
    std::mutex lifecycle_mutex_;
    std::atomic<bool> ota_busy_{false};

    static esp_err_t Dispatch(httpd_req_t* request);
    esp_err_t Handle(httpd_req_t* request);
    esp_err_t Move(httpd_req_t* request);
    esp_err_t Emotion(httpd_req_t* request);
    esp_err_t Canvas(httpd_req_t* request);
    esp_err_t CustomAction(httpd_req_t* request);
    esp_err_t CustomEmotion(httpd_req_t* request);
    esp_err_t WifiSettings(httpd_req_t* request);
    esp_err_t SettingsHandler(httpd_req_t* request);
    esp_err_t OtaUpload(httpd_req_t* request);
    esp_err_t SdCard(httpd_req_t* request);
};
