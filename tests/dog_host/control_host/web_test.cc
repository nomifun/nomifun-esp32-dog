#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <cJSON.h>
#include "web_control_parse.h"
// Permit direct invocation of private HTTP handlers while retaining the actual
// production class/Callbacks layout. Standard headers are included above.
#define private public
#include "web_control_server.h"
#undef private
#define ESP_LOGI(...) ((void)0)
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
using dog_http::Query;
using File = std::unique_ptr<FILE, decltype(&fclose)>;
constexpr size_t kMaxQuery = 12288;
int httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = status; return ESP_OK; }
int httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
int httpd_resp_set_hdr(httpd_req_t* req, const char* key, const char* value) { req->headers[key] = value; return ESP_OK; }
int httpd_resp_send(httpd_req_t* req, const char* value, size_t length) { req->response.assign(value, length); return ESP_OK; }
size_t httpd_req_get_url_query_len(httpd_req_t* req) { return req->query.size(); }
int httpd_req_get_url_query_str(httpd_req_t* req, char* out, size_t size) { if (req->query.size() >= size) return ESP_FAIL; std::strcpy(out, req->query.c_str()); return ESP_OK; }
int httpd_req_recv(httpd_req_t* req, char* out, size_t maximum) {
    const size_t count = std::min({maximum, req->body.size() - req->received, size_t(2)});
    std::memcpy(out, req->body.data() + req->received, count); req->received += count; return static_cast<int>(count);
}
class PetDog {
public:
    int actions = 0, customs = 0, explores = 0;
    bool ExecuteCustomAction(const std::string&) { ++customs; return true; }
    bool DoAction(const char*) { ++actions; return true; }
    bool SaveAngleSettings(const std::string&, const std::string&) { return true; }
    void SetFreeExplore(bool) { ++explores; }
};
class WS2812LED {
public:
    enum class Mode { kTurnOnOff, kFlash, kWater, kBreathe, kRainbow };
    int controls = 0;
    void Control(Mode, int, int, int, int) { ++controls; }
    void Control(const std::string&, const std::string&, int) { ++controls; }
};
class Settings {
public:
    static inline int writes = 0;
    Settings(const char*, bool) {}
    void SetInt(const char*, int) { ++writes; }
    void SetBool(const char*, bool) { ++writes; }
};
struct SsidItem { std::string ssid, password; };
class SsidManager {
public:
    bool save_ok = true;
    int saves = 0;
    std::vector<SsidItem> list;
    static SsidManager& GetInstance() { static SsidManager manager; return manager; }
    const std::vector<SsidItem>& GetSsidList() const { return list; }
    bool AddSsid(const std::string&, const std::string&) { ++saves; return save_ok; }
    bool RemoveSsid(int) { ++saves; return save_ok; }
};
struct EmotionConfig { std::string music, action, custom_action; bool loop = true; };
static EmotionConfig stored_config;
static int config_writes = 0;
bool EmotionName(const std::string& name) { return dog_http::Filename(name); }
bool FactoryEmotionName(const std::string&) { return false; }
bool ReadEmotionConfig(const std::string&, EmotionConfig& config, bool& exists) { config = stored_config; exists = true; return true; }
bool WriteEmotionConfig(const std::string&, const EmotionConfig& config) { stored_config = config; ++config_writes; return true; }
void AddEmotionConfig(cJSON*, const EmotionConfig&) {}
bool Exists(const std::string&, struct stat* info = nullptr) { if (info) info->st_size = 1; return true; }
struct dirent { const char* d_name; };
struct DIR {};
DIR* opendir(const char*) { return nullptr; }
dirent* readdir(DIR*) { return nullptr; }
int closedir(DIR*) { return 0; }
int unlink(const char*) { return 0; }
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
WebControlServer::WebControlServer(PetDog& dog, WS2812LED& led, Callbacks callbacks)
    : dog_(dog), led_(led), callbacks_(std::move(callbacks)) {}
WebControlServer::~WebControlServer() = default;

// INSERT_PRODUCTION_FUNCTIONS

int main() {
    int cases = 0, red = 17, green = 18, blue = 19, integer = 42;
    for (const char* value : {"", "-1", "+1", "1.0", " 1", "1 ", "100x", "99999999999999999999999999999999"}) {
        assert(!dog_http::Integer(value, 0, 100, integer) && integer == 42); ++cases;
    }
    assert(dog_http::Integer("0", 0, 100, integer) && integer == 0); ++cases;
    assert(dog_http::Integer("100", 0, 100, integer) && integer == 100); ++cases;
    assert(dog_http::Rgb("RGB(255,128,0)", red, green, blue) && red == 255 && green == 128 && blue == 0); ++cases;
    assert(dog_http::Rgb("rgb(0,1,255)", red, green, blue) && red == 0 && green == 1 && blue == 255); ++cases;
    for (const char* value : {"RGB(256,0,0)", "RGB(-1,0,0)", "RGB(1.5,0,0)", "RGB(99999999999999999999999,0,0)",
                              "RGB(1,2,3)tail", "RGB(1,2,3,4)", "RGB(1,,3)", "RGB(1,2,)", "RGB(1,2,3", "rgb(1,+2,3)"}) {
        red = 17; green = 18; blue = 19;
        assert(!dog_http::Rgb(value, red, green, blue) && red == 17 && green == 18 && blue == 19); ++cases;
    }
    Query query;
    assert(dog_http::ParseQuery("ssid=%E6%9C%BA%E5%99%A8%E7%8B%97&password=a%2Bb%26c", query));
    assert(query["ssid"] == "机器狗" && query["password"] == "a+b&c"); ++cases;
    for (const char* value : {"name=%", "name=%zz", "name=%00", "name=%0A", "name=a&name=b", "=value"}) {
        assert(!dog_http::ParseQuery(value, query)); ++cases;
    }
    std::string path;
    assert(dog_http::SdPath("/music/song.mp3", path) && path == "/sdcard/music/song.mp3"); ++cases;
    for (const char* value : {"../escape", "/../escape", "/music/../escape", "/music//escape", "/music\\escape"}) {
        assert(!dog_http::SdPath(value, path)); ++cases;
    }
    {
        PetDog moving_dog; WS2812LED moving_led;
        WebControlServer moving_server(moving_dog, moving_led, {});
        for (const char* value : {"dir=forward&brightness=abc", "dir=forward&brightness=101", "dir=forward&brightness=3.5",
                                  "dir=forward&color=RGB(9999999999999999999999,0,0)", "dir=forward&color=RGB(1,2,3)tail",
                                  "dir=missing&color=red", "dir=forward&effect=unknown", "dir=forward&dir=stand"}) {
            httpd_req_t req; req.query = value; moving_server.Move(&req);
            assert(req.status == "400 Bad Request" && !moving_dog.actions && !moving_led.controls); ++cases;
        }
        httpd_req_t valid; valid.query = "dir=forward&color=RGB(255,128,0)&brightness=100"; moving_server.Move(&valid);
        assert(valid.status == "200 OK" && moving_dog.actions == 1 && moving_led.controls == 1); ++cases;
    }
    PetDog dog; WS2812LED led;
    int volume_calls = 0, settings_calls = 0, displays = 0, music = 0, last_volume = -1;
    WebControlServer::Callbacks callbacks;
    callbacks.sd_mounted = [] { return true; };
    callbacks.set_volume = [&](int value) { ++volume_calls; last_volume = value; return true; };
    callbacks.settings_changed = [&] { ++settings_calls; return true; };
    callbacks.load_custom_emotion = [&](const std::string&) { ++displays; return true; };
    callbacks.play_music = [&](const std::string&, bool) { ++music; return true; };
    WebControlServer server(dog, led, callbacks);
    for (const char* value : {"abc", "", "-1", "101", "3.9", "%2B50", "9999999999999999999999999", "50tail"}) {
        httpd_req_t req; req.method = HTTP_POST; req.body = std::string("volume=") + value; req.content_len = req.body.size();
        server.SettingsHandler(&req);
        assert(req.status == "400 Bad Request" && !volume_calls && !settings_calls && !Settings::writes && !dog.explores); ++cases;
    }
    for (int value : {0, 50, 100}) {
        httpd_req_t req; req.method = HTTP_POST; req.body = "volume=" + std::to_string(value); req.content_len = req.body.size();
        server.SettingsHandler(&req); assert(req.status == "200 OK" && last_volume == value); ++cases;
    }
    stored_config = {"song.mp3", "stand", "hello", true};
    for (const char* command : {"cmd=play&name=test", "cmd=set_config&name=test&action=stand&custom_action=hello"}) {
        httpd_req_t req; req.query = command; server.CustomEmotion(&req);
        assert(req.status == "400 Bad Request" && !displays && !music && !dog.actions && !dog.customs && !config_writes); ++cases;
    }
    stored_config.action.clear();
    httpd_req_t custom; custom.query = "cmd=play&name=test"; server.CustomEmotion(&custom);
    assert(custom.status == "200 OK" && dog.customs == 1 && dog.actions == 0 && displays == 1 && music == 1); ++cases;
    stored_config.action = "stand"; stored_config.custom_action.clear();
    httpd_req_t standard; standard.query = "cmd=play&name=test"; server.CustomEmotion(&standard);
    assert(standard.status == "200 OK" && dog.customs == 1 && dog.actions == 1); ++cases;
    auto& wifi = SsidManager::GetInstance();
    for (bool saved : {false, true}) {
        wifi.save_ok = saved;
        httpd_req_t req; req.method = HTTP_POST; req.query = "ssid=Home&password=secret"; server.WifiSettings(&req);
        assert(req.status == (saved ? "200 OK" : "500 Internal Server Error")); ++cases;
    }
    assert(wifi.saves == 2);
    wifi.list.push_back({"Home", "secret"});
    for (bool saved : {false, true}) {
        wifi.save_ok = saved;
        httpd_req_t req; req.method = HTTP_DELETE; req.query = "index=0"; server.WifiSettings(&req);
        assert(req.status == (saved ? "200 OK" : "500 Internal Server Error")); ++cases;
    }
    const int before_invalid = wifi.saves;
    for (const char* index : {"", "-1", "0.0", "1", "0tail", "99999999999999999999"}) {
        httpd_req_t req; req.method = HTTP_DELETE; req.query = std::string("index=") + index; server.WifiSettings(&req);
        assert(req.status == "400 Bad Request" && wifi.saves == before_invalid); ++cases;
    }
    std::cout << cases << " HTTP numeric/path/query/settings/action-conflict/persistence cases passed\n";
}
