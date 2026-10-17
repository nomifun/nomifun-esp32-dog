#include "web_control_server.h"
#include "web_control_parse.h"
#include "pet_dog.h"
#include "ws2812_LED.h"
#include "settings.h"
#include <ssid_manager.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr const char* TAG = "WebControlServer";
constexpr size_t kMaxQuery = 12288;
constexpr size_t kMaxAction = 8192;
constexpr size_t kChunk = 4096;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
using File = std::unique_ptr<FILE, decltype(&fclose)>;
using dog_http::Query;

std::string PrintJson(cJSON* object) {
    char* raw = cJSON_PrintUnformatted(object);
    if (!raw) return "null";
    std::string value(raw);
    cJSON_free(raw);
    return value;
}
esp_err_t Reply(httpd_req_t* req, const char* status, const std::string& body,
                const char* type = "text/plain; charset=utf-8") {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, body.data(), body.size());
}
esp_err_t JsonReply(httpd_req_t* req, cJSON* body) {
    return Reply(req, "200 OK", PrintJson(body), "application/json");
}
esp_err_t BadRequest(httpd_req_t* req, const char* message) {
    return Reply(req, "400 Bad Request", message);
}
esp_err_t MethodNotAllowed(httpd_req_t* req) {
    return Reply(req, "405 Method Not Allowed", "Method not allowed");
}
bool GetQuery(httpd_req_t* req, Query& result) {
    size_t length = httpd_req_get_url_query_len(req);
    if (length > kMaxQuery) return false;
    if (length == 0) { result.clear(); return true; }
    std::vector<char> buffer(length + 1);
    if (httpd_req_get_url_query_str(req, buffer.data(), buffer.size()) != ESP_OK) return false;
    return dog_http::ParseQuery(buffer.data(), result);
}
bool ReadBody(httpd_req_t* req, std::string& result, size_t limit) {
    if (req->content_len == 0 || req->content_len > limit) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return false;
    }
    result.resize(req->content_len);
    size_t received = 0;
    unsigned timeouts = 0;
    while (received < result.size()) {
        int count = httpd_req_recv(req, result.data() + received, result.size() - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        if (count <= 0) {
            httpd_resp_set_hdr(req, "Connection", "close");
            return false;
        }
        received += count;
        timeouts = 0;
    }
    return true;
}
void Cors(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, HEAD, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-Sd-Path, Access-Control-Request-Private-Network");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "Content-Length, X-File-Size");
}
bool Exists(const std::string& path, struct stat* result = nullptr) {
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0) return false;
    if (result) *result = info;
    return true;
}
bool ReplaceFile(const std::string& temporary, const std::string& path) {
    struct stat info = {};
    if (!Exists(path, &info)) return rename(temporary.c_str(), path.c_str()) == 0;
    if (!S_ISREG(info.st_mode)) return false;
    // FatFs f_rename does not replace an existing destination. Preserve it until
    // the completed new file has taken its name; restore it if the swap fails.
    const std::string backup = path + ".previous-" + std::to_string(esp_random());
    if (rename(path.c_str(), backup.c_str()) != 0) return false;
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        if (rename(backup.c_str(), path.c_str()) != 0)
            ESP_LOGE(TAG, "File replacement failed; original retained at %s", backup.c_str());
        return false;
    }
    if (unlink(backup.c_str()) != 0) ESP_LOGW(TAG, "Old file retained at %s", backup.c_str());
    return true;
}
const char* kOtaPage = R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>机器狗本地固件升级</title><body><h1>本地固件升级</h1><p>选择为此 ESP32-S3 编译的应用程序 .bin（如 xiaozhi.bin）。不要上传合并整片 Flash 镜像。</p><input id="firmware" type="file" accept=".bin"><button id="upload">上传并升级</button><pre id="status"></pre><script>const file=document.getElementById('firmware'),button=document.getElementById('upload'),status=document.getElementById('status');button.onclick=async()=>{if(!file.files.length)return;button.disabled=true;status.textContent='正在上传固件…';try{const response=await fetch('/web_ota/upload',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:file.files[0]});status.textContent=await response.text();}catch(error){status.textContent=String(error);}finally{button.disabled=false;}};</script></body></html>)HTML";
struct EmotionConfig {
    std::string music, action, custom_action;
    bool loop = true;
};
bool EmotionName(const std::string& name) {
    return dog_http::Filename(name) && name.size() < 48 && name.find("..") == std::string::npos;
}
bool FactoryEmotionName(const std::string& name) {
    // Built-in emotion names; metadata writes are rejected for them.
    const char* factory[] = {"blink", "wink", "surprise", "happy", "aggrieved", "blame", "pitiful",
        "doubt", "comtempt", "mock", "quirky", "serious", "silly", "anger", "sadness", "asleep",
        "move1", "move2", "move3", "move4", "firework"};
    return std::any_of(std::begin(factory), std::end(factory), [&name](const char* item) { return name == item; });
}
bool ReadEmotionConfig(const std::string& name, EmotionConfig& config, bool& exists) {
    const std::string path = "/sdcard/custom_emo/" + name + ".json";
    struct stat info = {};
    exists = Exists(path, &info);
    if (!exists) return true;
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 4096) return false;
    File file(fopen(path.c_str(), "rb"), fclose);
    if (!file) return false;
    std::string content(info.st_size, '\0');
    if (fread(content.data(), 1, content.size(), file.get()) != content.size()) return false;
    Json json(cJSON_ParseWithLengthOpts(content.c_str(), content.size() + 1, nullptr, true), cJSON_Delete);
    if (!json || !cJSON_IsObject(json.get())) return false;
    const std::pair<const char*, std::string*> fields[] = {
        {"music", &config.music}, {"action", &config.action}, {"custom_action", &config.custom_action}};
    for (const auto& field : fields) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(json.get(), field.first);
        if (item && !cJSON_IsString(item)) return false;
        if (item) *field.second = item->valuestring;
    }
    cJSON* loop = cJSON_GetObjectItemCaseSensitive(json.get(), "loop");
    if (loop && !cJSON_IsBool(loop)) return false;
    if (loop) config.loop = cJSON_IsTrue(loop);
    return true;
}
void AddEmotionConfig(cJSON* json, const EmotionConfig& config) {
    cJSON_AddStringToObject(json, "music", config.music.c_str());
    cJSON_AddStringToObject(json, "action", config.action.c_str());
    cJSON_AddStringToObject(json, "custom_action", config.custom_action.c_str());
    cJSON_AddBoolToObject(json, "loop", config.loop);
}
bool WriteEmotionConfig(const std::string& name, const EmotionConfig& config) {
    struct stat info = {};
    if (mkdir("/sdcard/custom_emo", 0775) != 0 &&
        (!Exists("/sdcard/custom_emo", &info) || !S_ISDIR(info.st_mode))) return false;
    Json json(cJSON_CreateObject(), cJSON_Delete);
    AddEmotionConfig(json.get(), config);
    const std::string content = PrintJson(json.get());
    const std::string path = "/sdcard/custom_emo/" + name + ".json";
    const std::string temporary = path + ".tmp-" + std::to_string(esp_random());
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0664);
    File file(fd < 0 ? nullptr : fdopen(fd, "wb"), fclose);
    if (!file) { if (fd >= 0) { close(fd); unlink(temporary.c_str()); } return false; }
    const bool wrote = fwrite(content.data(), 1, content.size(), file.get()) == content.size();
    FILE* raw = file.release();
    const bool closed = fclose(raw) == 0;
    if (!wrote || !closed || !ReplaceFile(temporary, path)) {
        unlink(temporary.c_str());
        return false;
    }
    return true;
}
bool FormBoolean(const Query& form, const char* key, bool fallback) {
    auto it = form.find(key);
    if (it == form.end()) return fallback;
    std::string value = it->second;
    for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (value == "1" || value == "true" || value == "on" || value == "yes") return true;
    if (value == "0" || value == "false" || value == "off" || value == "no") return false;
    return fallback;
}
void RebootAfterResponse(void*) {
    // Queued work runs only after the upload handler and response have finished.
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
} // namespace

WebControlServer::WebControlServer(PetDog& dog, WS2812LED& led, Callbacks callbacks)
    : dog_(dog), led_(led), callbacks_(std::move(callbacks)) {
    ESP_LOGI(TAG, "WebControlServer constructor called");
}
WebControlServer::~WebControlServer() { Stop(); }

esp_err_t WebControlServer::Start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (server_) return ESP_OK;
    ESP_LOGI(TAG, "Starting WebControlServer...");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 18;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) { server_ = nullptr; return err; }
    const char* routes[] = {"/", "/move", "/emotion", "/canvas", "/custom_action", "/custom_emo",
        "/wifi_settings", "/settings", "/remote_action", "/web_ota", "/web_ota/upload",
        "/sdcard", "/sdcard/upload", "/sdcard/mkdir", "/sdcard/download", "/sdcard/format"};
    for (const char* route : routes) {
        httpd_uri_t uri = {};
        uri.uri = route;
        uri.method = static_cast<httpd_method_t>(HTTP_ANY);
        uri.handler = Dispatch;
        uri.user_ctx = this;
        err = httpd_register_uri_handler(server_, &uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s", route, esp_err_to_name(err));
            httpd_stop(server_);
            server_ = nullptr;
            return err;
        }
    }
    ESP_LOGI(TAG, "HTTP server started successfully");
    return ESP_OK;
}
void WebControlServer::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (server_) {
        ESP_LOGI(TAG, "Stopping HTTP server");
        httpd_stop(server_);
        server_ = nullptr;
    }
}
esp_err_t WebControlServer::Dispatch(httpd_req_t* req) {
    auto* self = static_cast<WebControlServer*>(req->user_ctx);
    if (!self) return Reply(req, "500 Internal Server Error", "Invalid user context");
    return self->Handle(req);
}
esp_err_t WebControlServer::Handle(httpd_req_t* req) {
    Cors(req);
    if (req->method == HTTP_OPTIONS) return Reply(req, "200 OK", "");
    std::string path(req->uri);
    path.erase(path.find('?') == std::string::npos ? path.size() : path.find('?'));
    if (path == "/move" || path == "/remote_action") return Move(req);
    if (path == "/emotion") return Emotion(req);
    if (path == "/canvas") return Canvas(req);
    if (path == "/custom_action") return CustomAction(req);
    if (path == "/custom_emo") return CustomEmotion(req);
    if (path == "/wifi_settings") return WifiSettings(req);
    if (path == "/settings") return SettingsHandler(req);
    if (path == "/web_ota/upload") return OtaUpload(req);
    if (path == "/web_ota") {
        if (req->method != HTTP_GET) return MethodNotAllowed(req);
        return Reply(req, "200 OK", kOtaPage, "text/html; charset=utf-8");
    }
    if (path.compare(0, 7, "/sdcard") == 0) return SdCard(req);
    if (path == "/" && req->method == HTTP_GET) {
        return Reply(req, "200 OK", "<!doctype html><meta charset=utf-8><title>机器狗</title><h1>机器狗控制服务</h1><p><a href=/web_ota>本地固件升级</a></p><p>HTTP API: /move /emotion /canvas /custom_action /wifi_settings /settings</p>", "text/html; charset=utf-8");
    }
    return MethodNotAllowed(req);
}

esp_err_t WebControlServer::Move(httpd_req_t* req) {
    if (req->method != HTTP_GET) return MethodNotAllowed(req);
    Query query;
    if (!GetQuery(req, query) || query.empty()) return BadRequest(req, "Missing or invalid query");
    std::string action = query["dir"], effect = query["effect"], color = query["color"];
    int brightness = 50;
    if (query.count("brightness") && !dog_http::Integer(query["brightness"], 0, 100, brightness))
        return BadRequest(req, "Invalid brightness");
    if (!effect.empty() && effect != "flash_light" && effect != "water_light" &&
        effect != "breathe_light" && effect != "rainbow_light") return BadRequest(req, "Unknown light effect");
    int red = 0, green = 0, blue = 0;
    bool rgb = color.compare(0, 4, "RGB(") == 0 || color.compare(0, 4, "rgb(") == 0;
    if (rgb && !dog_http::Rgb(color, red, green, blue)) return BadRequest(req, "Invalid RGB color");
    if (!color.empty() && !rgb) {
        const char* colors[] = {"red", "orange", "yellow", "green", "blue", "purple", "white", "off",
            "红灯", "橙灯", "黄灯", "绿灯", "蓝灯", "紫灯", "白灯", "关灯"};
        if (std::none_of(std::begin(colors), std::end(colors), [&color](const char* value) { return color == value; }))
            return BadRequest(req, "Unknown light color");
    }
    // Validate the complete request before starting effects or moving a servo.
    if (!action.empty() && action != "light_effect" && action != "save_angles") {
        const char* allowed[] = {"forward", "back", "left", "right", "stop", "sitdown", "stand", "sleep",
            "stretch", "scratching", "swing", "petwave", "jump", "wake"};
        if (std::none_of(std::begin(allowed), std::end(allowed), [&action](const char* value) { return action == value; }))
            return BadRequest(req, "Unknown direction/action");
    }
    if (action == "light_effect" && effect.empty()) return BadRequest(req, "Missing effect");
    if (action.empty() && color.empty() && effect.empty()) return BadRequest(req, "Missing action or light control");
    if (action == "save_angles") {
        if (query["angles"].empty()) return BadRequest(req, "Missing angles");
        std::string posture = query["action_type"];
        if (!posture.empty() && posture != "stand" && posture != "sleep" && posture != "sit")
            return BadRequest(req, "Invalid action_type");
        if (!dog_.SaveAngleSettings(query["angles"], posture.empty() ? "stand" : posture))
            return BadRequest(req, "Invalid angle settings");
        if (!posture.empty() && !dog_.DoAction(posture == "sit" ? "sitdown" : posture.c_str()))
            return Reply(req, "503 Service Unavailable", "Failed to execute posture");
    } else if (!action.empty() && action != "light_effect") {
        if (action == "wake") {
            if (!callbacks_.wake || !callbacks_.wake()) return Reply(req, "503 Service Unavailable", "Wake unavailable");
            action = "stand";
        } else if (action == "sleep") {
            if (!callbacks_.sleep || !callbacks_.sleep()) return Reply(req, "503 Service Unavailable", "Sleep unavailable");
        }
        if (action == "forward") action = "walk_forward";
        else if (action == "back") action = "walk_back";
        else if (action == "left") action = "turn_left";
        else if (action == "right") action = "turn_right";
        if (!dog_.DoAction(action.c_str())) return Reply(req, "503 Service Unavailable", "Failed to execute action");
    }
    if (!color.empty()) {
        if (rgb) led_.Control(WS2812LED::Mode::kTurnOnOff, red, green, blue, brightness);
        else led_.Control("turn_on_off", color, brightness);
    }
    if (!effect.empty()) {
        if (rgb) {
            WS2812LED::Mode mode = effect == "flash_light" ? WS2812LED::Mode::kFlash :
                effect == "water_light" ? WS2812LED::Mode::kWater :
                effect == "breathe_light" ? WS2812LED::Mode::kBreathe : WS2812LED::Mode::kRainbow;
            led_.Control(mode, red, green, blue, brightness);
        } else led_.Control(effect, color.empty() ? "white" : color, brightness);
    }
    return Reply(req, "200 OK", "OK");
}

esp_err_t WebControlServer::Emotion(httpd_req_t* req) {
    if (req->method != HTTP_GET) return MethodNotAllowed(req);
    Query query;
    if (!GetQuery(req, query) || query["emotion"].empty()) return BadRequest(req, "Missing emotion");
    bool ok = query["emotion"] == "random" ? callbacks_.random_emotion && callbacks_.random_emotion() :
        callbacks_.set_emotion && callbacks_.set_emotion(query["emotion"]);
    if (!ok) return Reply(req, "503 Service Unavailable", "Display unavailable or unknown emotion");
    return Reply(req, "200 OK", "OK");
}

esp_err_t WebControlServer::Canvas(httpd_req_t* req) {
    // Bitmap upload is not supported: POST is rejected with 410 Gone.
    if (req->method == HTTP_POST) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return Reply(req, "410 Gone", "Canvas feature removed");
    }
    if (req->method == HTTP_HEAD) return Reply(req, "200 OK", "");
    if (req->method != HTTP_GET && req->method != HTTP_DELETE) return MethodNotAllowed(req);
    Query query;
    if (!GetQuery(req, query)) return BadRequest(req, "Invalid query");
    if (!callbacks_.sd_mounted || !callbacks_.sd_mounted()) return BadRequest(req, "SD card not detected");
    if (req->method == HTTP_GET && dog_http::Enabled(query["list"])) {
        Json list(cJSON_CreateArray(), cJSON_Delete);
        DIR* directory = opendir("/sdcard/custom_emo");
        if (directory) {
            while (dirent* entry = readdir(directory)) {
                if (!dog_http::Filename(entry->d_name)) continue;
                struct stat info = {};
                if (Exists(std::string("/sdcard/custom_emo/") + entry->d_name, &info) && S_ISREG(info.st_mode))
                    cJSON_AddItemToArray(list.get(), cJSON_CreateString(entry->d_name));
            }
            closedir(directory);
        }
        return JsonReply(req, list.get());
    }
    if (!dog_http::Filename(query["name"])) return BadRequest(req, "Missing or invalid name parameter");
    const std::string path = "/sdcard/custom_emo/" + query["name"];
    if (!Exists(path)) return Reply(req, "404 Not Found", "File not found");
    if (req->method == HTTP_DELETE || dog_http::Enabled(query["delete"])) {
        if (unlink(path.c_str()) != 0) return Reply(req, "500 Internal Server Error", "Failed to delete file");
        return Reply(req, "200 OK", "OK");
    }
    if (dog_http::Enabled(query["load"])) {
        if (!callbacks_.load_custom_emotion || !callbacks_.load_custom_emotion(query["name"]))
            return Reply(req, "503 Service Unavailable", "Display unavailable or invalid custom emotion");
        return Reply(req, "200 OK", "OK");
    }
    return BadRequest(req, "Invalid request");
}

esp_err_t WebControlServer::CustomAction(httpd_req_t* req) {
    Query query;
    if (!GetQuery(req, query)) return BadRequest(req, "Invalid query");
    if (req->method == HTTP_GET && query["name"].empty() && query["action"].empty()) {
        Json list(cJSON_CreateArray(), cJSON_Delete);
        if (callbacks_.sd_mounted && callbacks_.sd_mounted()) {
            for (const auto& name : dog_.ListCustomActions()) cJSON_AddItemToArray(list.get(), cJSON_CreateString(name.c_str()));
        }
        return JsonReply(req, list.get());
    }
    if (req->method != HTTP_GET && req->method != HTTP_POST && req->method != HTTP_DELETE) return MethodNotAllowed(req);
    if (!callbacks_.sd_mounted || !callbacks_.sd_mounted()) return BadRequest(req, "SD card not detected");
    const std::string name = query["action"].empty() ? query["name"] : query["action"];
    if (!dog_http::Filename(name)) return BadRequest(req, "Missing action name");
    if (req->method == HTTP_GET) {
        if (!query["action"].empty()) {
            if (!dog_.ExecuteCustomAction(name)) return Reply(req, "404 Not Found", "Failed to execute action");
            return Reply(req, "200 OK", "OK");
        }
        std::string content = dog_.LoadCustomAction(name);
        if (content.empty()) return Reply(req, "404 Not Found", "Action not found");
        return Reply(req, "200 OK", content, "application/json");
    }
    if (req->method == HTTP_DELETE) {
        if (!dog_.DeleteCustomAction(name)) return Reply(req, "404 Not Found", "Failed to delete action");
        return Reply(req, "200 OK", "OK");
    }
    if (req->content_len == 0 || req->content_len > kMaxAction) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return BadRequest(req, "Invalid content length");
    }
    std::string body;
    if (!ReadBody(req, body, kMaxAction)) return BadRequest(req, "Failed to receive data");
    if (!dog_.SaveCustomAction(name, body)) return BadRequest(req, "Failed to save action");
    return Reply(req, "200 OK", "OK");
}

esp_err_t WebControlServer::CustomEmotion(httpd_req_t* req) {
    // Handles /sdcard/custom_emo queries: cmd/name/music/action/custom_action/loop.
    Query query;
    if (!GetQuery(req, query)) return BadRequest(req, "Invalid query");
    const std::string command = query["cmd"].empty() ? "list" : query["cmd"];
    const std::string name = query["name"];
    if (!callbacks_.sd_mounted || !callbacks_.sd_mounted())
        return Reply(req, "503 Service Unavailable", "{\"ok\":false,\"error\":\"SD card not mounted\"}", "application/json");
    auto success = [req](const char* key) {
        Json json(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddBoolToObject(json.get(), "ok", true);
        cJSON_AddBoolToObject(json.get(), key, true);
        return JsonReply(req, json.get());
    };
    if (command == "list") {
        Json json(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddBoolToObject(json.get(), "ok", true);
        cJSON_AddStringToObject(json.get(), "dir", "/custom_emo");
        cJSON* items = cJSON_AddArrayToObject(json.get(), "items");
        DIR* directory = opendir("/sdcard/custom_emo");
        if (directory) {
            while (dirent* entry = readdir(directory)) {
                const std::string file = entry->d_name;
                if (file.size() <= 4 || file.substr(file.size() - 4) != ".gif" || !dog_http::Filename(file)) continue;
                const std::string item_name = file.substr(0, file.size() - 4);
                if (!EmotionName(item_name)) continue;
                struct stat info = {};
                if (!Exists("/sdcard/custom_emo/" + file, &info) || !S_ISREG(info.st_mode)) continue;
                EmotionConfig config;
                bool exists = false;
                if (!ReadEmotionConfig(item_name, config, exists)) {
                    closedir(directory);
                    return Reply(req, "500 Internal Server Error", "Invalid custom emotion config");
                }
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", item_name.c_str());
                cJSON_AddStringToObject(item, "file", file.c_str());
                cJSON_AddNumberToObject(item, "size", info.st_size);
                AddEmotionConfig(item, config);
                cJSON_AddItemToArray(items, item);
            }
            closedir(directory);
        }
        return JsonReply(req, json.get());
    }
    if (command == "reload") {
        // There is no shared SD emotion/config cache. If a name is supplied,
        // reload the visible GIF from the card.
        if (!name.empty() && (!EmotionName(name) || !callbacks_.load_custom_emotion || !callbacks_.load_custom_emotion(name)))
            return Reply(req, "503 Service Unavailable", "Display unavailable or invalid custom emotion");
        return success("reloaded");
    }
    if (!EmotionName(name)) return BadRequest(req, "Invalid name");
    EmotionConfig config;
    bool exists = false;
    if (!ReadEmotionConfig(name, config, exists)) return Reply(req, "500 Internal Server Error", "Invalid custom emotion config");
    if (command == "get_config") {
        Json json(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddBoolToObject(json.get(), "ok", true);
        cJSON_AddStringToObject(json.get(), "name", name.c_str());
        cJSON_AddStringToObject(json.get(), "kind", FactoryEmotionName(name) ? "replace" : "custom");
        cJSON_AddBoolToObject(json.get(), "exists", Exists("/sdcard/custom_emo/" + name + ".gif"));
        AddEmotionConfig(json.get(), config);
        return JsonReply(req, json.get());
    }
    if (command == "set_config") {
        // Writes <name>.json and refuses reserved factory names. Metadata may
        // be uploaded before the GIF.
        if (FactoryEmotionName(name))
            return Reply(req, "400 Bad Request", "{\"ok\":false,\"error\":\"save failed (custom names only)\"}", "application/json");
        config.music = query["music"];
        config.action = query["action"];
        config.custom_action = query["custom_action"];
        config.loop = FormBoolean(query, "loop", true);
        if (!config.action.empty() && !config.custom_action.empty())
            return BadRequest(req, "Choose action or custom_action, not both");
        if (config.music.size() >= 96 || config.music.find("..") != std::string::npos ||
            config.music.find('\\') != std::string::npos ||
            (!config.action.empty() && !dog_http::Filename(config.action)) ||
            (!config.custom_action.empty() && !EmotionName(config.custom_action))) return BadRequest(req, "Invalid config token");
        if (!WriteEmotionConfig(name, config)) return Reply(req, "500 Internal Server Error", "Failed to save custom emotion config");
        return success("saved");
    }
    if (command == "delete") {
        bool found = false;
        for (const char* suffix : {".gif", ".json"}) {
            const std::string path = "/sdcard/custom_emo/" + name + suffix;
            if (Exists(path)) {
                found = true;
                if (unlink(path.c_str()) != 0) return Reply(req, "500 Internal Server Error", "Failed to delete custom emotion");
            }
        }
        if (!found) return Reply(req, "404 Not Found", "Custom emotion not found");
        return success("deleted");
    }
    if (command == "play") {
        // PetDog accepts one command at a time: enqueueing both would silently
        // supersede the custom action. Reject existing ambiguous metadata before
        // starting its display, music or servo effects.
        if (!config.action.empty() && !config.custom_action.empty())
            return BadRequest(req, "Choose action or custom_action, not both");
        if (!config.music.empty() && !callbacks_.play_music)
            return Reply(req, "503 Service Unavailable", "Music backend unavailable");
        if (!callbacks_.load_custom_emotion || !callbacks_.load_custom_emotion(name))
            return Reply(req, "503 Service Unavailable", "Display unavailable or invalid custom emotion");
        if (!config.music.empty() && !callbacks_.play_music(config.music, config.loop))
            return Reply(req, "503 Service Unavailable", "Failed to play custom emotion music");
        if (!config.custom_action.empty() && !dog_.ExecuteCustomAction(config.custom_action))
            return Reply(req, "503 Service Unavailable", "Failed to execute custom action");
        if (!config.action.empty() && !dog_.DoAction(config.action.c_str()))
            return Reply(req, "503 Service Unavailable", "Failed to execute action");
        return success("playing");
    }
    return Reply(req, "400 Bad Request", "{\"ok\":false,\"error\":\"unknown cmd\"}", "application/json");
}

esp_err_t WebControlServer::WifiSettings(httpd_req_t* req) {
    auto& manager = SsidManager::GetInstance();
    if (req->method == HTTP_GET) {
        Json list(cJSON_CreateArray(), cJSON_Delete);
        for (const auto& item : manager.GetSsidList()) {
            cJSON* row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "ssid", item.ssid.c_str());
            cJSON_AddStringToObject(row, "password", item.password.c_str());
            cJSON_AddItemToArray(list.get(), row);
        }
        return JsonReply(req, list.get());
    }
    Query query;
    if (!GetQuery(req, query)) return BadRequest(req, "Invalid query");
    if (req->method == HTTP_POST) {
        if (query["ssid"].empty() || query["ssid"].size() > 32 || query["password"].size() > 64)
            return BadRequest(req, "Missing or invalid ssid/password");
        if (!manager.AddSsid(query["ssid"], query["password"]))
            return Reply(req, "500 Internal Server Error", "Failed to save Wi-Fi credentials");
    } else if (req->method == HTTP_DELETE) {
        int index = 0;
        if (!dog_http::Integer(query["index"], 0, INT_MAX, index) ||
            static_cast<size_t>(index) >= manager.GetSsidList().size()) return BadRequest(req, "Invalid index");
        if (!manager.RemoveSsid(index))
            return Reply(req, "500 Internal Server Error", "Failed to delete Wi-Fi credentials");
    } else return MethodNotAllowed(req);
    return Reply(req, "200 OK", "OK");
}

esp_err_t WebControlServer::SettingsHandler(httpd_req_t* req) {
    // POST + OPTIONS; urlencoded body, 1..4096 bytes.
    if (req->method != HTTP_POST) return MethodNotAllowed(req);
    std::string body;
    if (!ReadBody(req, body, 4096)) return BadRequest(req, "Invalid content length or receive failed");
    Query form;
    if (!dog_http::ParseQuery(body, form)) return BadRequest(req, "Invalid form data");
    int mode = 1, posture = 0, volume = 50;
    if (form.count("music_mode")) {
        if (form["music_mode"] == "single") mode = 0;
        else if (form["music_mode"] == "random") mode = 1;
        else if (form["music_mode"] == "sequence") mode = 2;
        else return BadRequest(req, "Invalid music_mode");
    }
    if (form.count("music_posture")) {
        if (form["music_posture"] == "stand") posture = 0;
        else if (form["music_posture"] == "sleep") posture = 1;
        else if (form["music_posture"] == "sit") posture = 2;
        else return BadRequest(req, "Invalid music_posture");
    }
    if (form.count("volume")) {
        if (!dog_http::Integer(form["volume"], 0, 100, volume)) return BadRequest(req, "Invalid volume");
    }
    bool boot = FormBoolean(form, "boot_animation", true), charge = FormBoolean(form, "charging_led", true);
    bool english = FormBoolean(form, "lang_en", false), explore = FormBoolean(form, "free_explore", false);
    if (!callbacks_.set_volume || !callbacks_.settings_changed)
        return Reply(req, "503 Service Unavailable", "Settings backend unavailable");
    if (!callbacks_.set_volume(volume)) return Reply(req, "503 Service Unavailable", "Audio unavailable");
    {
        // The display setters persist all these values under the "display"
        // namespace, not "dog"; Settings commits on destruction before live reload.
        Settings settings("display", true);
        settings.SetInt("music_mode", mode);
        settings.SetInt("music_posture", posture);
        settings.SetBool("boot_animation", boot);
        settings.SetBool("charging_led", charge);
        settings.SetBool("lang_en", english);
        settings.SetBool("free_explore", explore);
    }
    dog_.SetFreeExplore(explore);
    if (!callbacks_.settings_changed()) return Reply(req, "503 Service Unavailable", "Settings saved, live reload failed");
    ESP_LOGI(TAG, "Basic settings saved: mode=%d posture=%d vol=%d boot=%d charge=%d en=%d explore=%d",
             mode, posture, volume, boot, charge, english, explore);
    return Reply(req, "200 OK", "OK");
}

esp_err_t WebControlServer::OtaUpload(httpd_req_t* req) {
    if (req->method != HTTP_POST) return Reply(req, "405 Method Not Allowed", "{\"ok\":false,\"error\":\"POST only\"}", "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    auto error = [req](const char* status, const char* message) {
        Json json(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddBoolToObject(json.get(), "ok", false);
        cJSON_AddStringToObject(json.get(), "error", message);
        return Reply(req, status, PrintJson(json.get()), "application/json");
    };
    if (req->content_len == 0) return error("400 Bad Request", "missing Content-Length");
    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!partition || !running || partition->address == running->address)
        return error("500 Internal Server Error", "no ota partition");
    if (req->content_len > partition->size) return error("413 Payload Too Large", "firmware too large");
    char content_type[80] = {};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK &&
        strstr(content_type, "multipart/")) return error("400 Bad Request", "send a raw app binary, not multipart");
    constexpr size_t kHeaderSize = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (req->content_len < kHeaderSize) return error("400 Bad Request", "invalid firmware image");
    bool expected = false;
    if (!ota_busy_.compare_exchange_strong(expected, true)) return error("409 Conflict", "busy");
    // On every error path abort the OTA handle and keep the current boot slot.
    esp_ota_handle_t handle = 0;
    bool begun = false;
    auto fail = [&](const char* message) {
        if (begun) esp_ota_abort(handle);
        ota_busy_.store(false);
        return error("400 Bad Request", message);
    };
    ESP_LOGI("WebOta", "Local OTA upload start, size=%d, partition=%s", static_cast<int>(req->content_len), partition->label);
    std::array<uint8_t, kChunk> buffer;
    size_t first = 0;
    unsigned timeouts = 0;
    // TCP packet boundaries do not coincide with image headers.
    while (first < kHeaderSize) {
        int received = httpd_req_recv(req, reinterpret_cast<char*>(buffer.data()) + first, kHeaderSize - first);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        if (received <= 0) return fail("receive failed");
        first += received;
        timeouts = 0;
    }
    esp_image_header_t header;
    esp_app_desc_t descriptor;
    memcpy(&header, buffer.data(), sizeof(header));
    memcpy(&descriptor, buffer.data() + sizeof(header) + sizeof(esp_image_segment_header_t), sizeof(descriptor));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32S3 ||
        descriptor.magic_word != ESP_APP_DESC_MAGIC_WORD) return fail("invalid firmware image");
    if (esp_ota_begin(partition, req->content_len, &handle) != ESP_OK) return fail("ota begin failed");
    begun = true;
    if (esp_ota_write(handle, buffer.data(), first) != ESP_OK) return fail("ota write failed");
    size_t written = first;
    while (written < req->content_len) {
        int received = httpd_req_recv(req, reinterpret_cast<char*>(buffer.data()), std::min(buffer.size(), req->content_len - written));
        if (received == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        if (received <= 0) return fail("receive failed");
        timeouts = 0;
        if (esp_ota_write(handle, buffer.data(), received) != ESP_OK) return fail("ota write failed");
        written += received;
    }
    esp_err_t ended = esp_ota_end(handle);
    begun = false; // esp_ota_end releases its handle even on validation failure.
    if (ended != ESP_OK) return fail("invalid firmware image");
    // esp_ota_end checks full image integrity and compatibility; only then is
    // the validated inactive slot made bootable.
    if (esp_ota_set_boot_partition(partition) != ESP_OK) return fail("set boot partition failed");
    char version[sizeof(descriptor.version) + 1] = {};
    memcpy(version, descriptor.version, sizeof(descriptor.version));
    Json result(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddBoolToObject(result.get(), "ok", true);
    cJSON_AddBoolToObject(result.get(), "rebooting", true);
    cJSON_AddStringToObject(result.get(), "version", version);
    esp_err_t sent = Reply(req, "200 OK", PrintJson(result.get()), "application/json");
    if (sent == ESP_OK) {
        // No reboot from inside receive/write or before httpd_resp_send finishes.
        if (httpd_queue_work(req->handle, RebootAfterResponse, nullptr) != ESP_OK) {
            ESP_LOGE("WebOta", "Firmware installed; failed to queue reboot, restart manually");
            ota_busy_.store(false);
        }
    } else ota_busy_.store(false);
    return sent;
}

esp_err_t WebControlServer::SdCard(httpd_req_t* req) {
    if (!callbacks_.sd_mounted || !callbacks_.sd_mounted())
        return Reply(req, "503 Service Unavailable", "SD card not mounted");
    Query query;
    if (!GetQuery(req, query)) return BadRequest(req, "Invalid query");
    std::string uri(req->uri);
    uri = uri.substr(0, uri.find('?'));
    if (uri == "/sdcard/format") {
        // A full-card format could erase tracks the user wants to keep, so it
        // is not offered here.
        return Reply(req, "501 Not Implemented", "SD card format is not supported");
    }
    std::string requested = query["path"];
    if (requested.empty()) {
        size_t length = httpd_req_get_hdr_value_len(req, "X-Sd-Path");
        if (length > 0 && length < 240) {
            std::vector<char> raw(length + 1);
            if (httpd_req_get_hdr_value_str(req, "X-Sd-Path", raw.data(), raw.size()) == ESP_OK &&
                !dog_http::Decode(raw.data(), requested)) return BadRequest(req, "Invalid path");
        }
    }
    if (requested.empty() && uri == "/sdcard") requested = "/";
    std::string path;
    if (!dog_http::SdPath(requested, path)) return BadRequest(req, "Invalid path");
    if (uri == "/sdcard/mkdir") {
        if (req->method != HTTP_POST) return MethodNotAllowed(req);
        struct stat info = {};
        if (mkdir(path.c_str(), 0775) != 0 && (!Exists(path, &info) || !S_ISDIR(info.st_mode)))
            return Reply(req, "500 Internal Server Error", "Mkdir failed");
        return Reply(req, "200 OK", "OK");
    }
    if (uri == "/sdcard/upload") {
        if (req->method != HTTP_POST) return MethodNotAllowed(req);
        if (path == "/sdcard/" || req->content_len == 0) return BadRequest(req, "Empty body or invalid path");
        // Upload to a sibling temporary file, rename only after all bytes arrive.
        const std::string temporary = path + ".upload-" + std::to_string(esp_random());
        int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0664);
        File output(fd < 0 ? nullptr : fdopen(fd, "wb"), fclose);
        if (fd >= 0 && !output) { close(fd); unlink(temporary.c_str()); }
        if (!output) return Reply(req, "500 Internal Server Error", "Create file failed");
        std::array<char, kChunk> buffer;
        size_t received = 0;
        unsigned timeouts = 0;
        while (received < req->content_len) {
            int count = httpd_req_recv(req, buffer.data(), std::min(buffer.size(), req->content_len - received));
            if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
            if (count <= 0 || fwrite(buffer.data(), 1, count, output.get()) != static_cast<size_t>(count)) {
                output.reset();
                unlink(temporary.c_str());
                return BadRequest(req, "Incomplete upload or write failed");
            }
            received += count;
            timeouts = 0;
        }
        FILE* raw_file = output.release();
        if (fclose(raw_file) != 0 || !ReplaceFile(temporary, path)) {
            unlink(temporary.c_str());
            return Reply(req, "500 Internal Server Error", "Write failed");
        }
        return Reply(req, "200 OK", "OK");
    }
    struct stat info = {};
    if (!Exists(path, &info)) return Reply(req, "404 Not Found", "File not found");
    if (req->method == HTTP_DELETE && uri == "/sdcard") {
        if (path == "/sdcard/") return BadRequest(req, "Cannot delete root");
        // unlink never recursively deletes an unknown tree.
        int result = S_ISDIR(info.st_mode) ? rmdir(path.c_str()) : unlink(path.c_str());
        if (result != 0) return Reply(req, "500 Internal Server Error", "Delete failed (directory must be empty)");
        return Reply(req, "200 OK", "OK");
    }
    if (req->method != HTTP_GET && req->method != HTTP_HEAD) return MethodNotAllowed(req);
    if (uri == "/sdcard/download") {
        if (!S_ISREG(info.st_mode)) return BadRequest(req, "Invalid path");
        File input(fopen(path.c_str(), "rb"), fclose);
        if (!input) return Reply(req, "500 Internal Server Error", "Open failed");
        httpd_resp_set_type(req, "application/octet-stream");
        const std::string size = std::to_string(info.st_size);
        httpd_resp_set_hdr(req, "X-File-Size", size.c_str());
        if (req->method == HTTP_HEAD) return Reply(req, "200 OK", "", "application/octet-stream");
        std::array<char, kChunk> buffer;
        size_t count = 0;
        while ((count = fread(buffer.data(), 1, buffer.size(), input.get())) > 0) {
            if (httpd_resp_send_chunk(req, buffer.data(), count) != ESP_OK) return ESP_FAIL;
        }
        if (ferror(input.get())) return ESP_FAIL;
        return httpd_resp_send_chunk(req, nullptr, 0);
    }
    if (!S_ISDIR(info.st_mode)) return BadRequest(req, "Not a directory");
    DIR* directory = opendir(path.c_str());
    if (!directory) return Reply(req, "404 Not Found", "Directory not found");
    Json output(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(output.get(), "path", requested.c_str());
    uint64_t total = 0, free = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &free) == ESP_OK) {
        cJSON_AddNumberToObject(output.get(), "total", static_cast<double>(total));
        cJSON_AddNumberToObject(output.get(), "free", static_cast<double>(free));
    }
    cJSON* entries = cJSON_AddArrayToObject(output.get(), "entries");
    while (dirent* entry = readdir(directory)) {
        if (!dog_http::Filename(entry->d_name)) continue;
        struct stat item = {};
        if (!Exists(path + "/" + entry->d_name, &item)) continue;
        cJSON* row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "name", entry->d_name);
        cJSON_AddStringToObject(row, "type", S_ISDIR(item.st_mode) ? "dir" : "file");
        cJSON_AddNumberToObject(row, "size", item.st_size);
        cJSON_AddNumberToObject(row, "mtime", item.st_mtime);
        cJSON_AddItemToArray(entries, row);
    }
    closedir(directory);
    return JsonReply(req, output.get());
}
