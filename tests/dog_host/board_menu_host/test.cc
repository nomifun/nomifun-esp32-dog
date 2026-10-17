#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "device_state.h"
#include "actual_enums.inc"

int cases = 0;
bool main_context = true;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void MainOnly() { Require(main_context, "Menu callback executed service/UI work before main dispatch"); }
void Pass(const char* message) { ++cases; std::cout << "PASS " << message << '\n'; }
constexpr int MAIN_EVENT_SCHEDULE = 1;
void xEventGroupSetBits(int, int) {}
#define TAG "Application"
#define ESP_LOGW(...) ((void)0)

struct Protocol {
    bool opened = false;
    int opens = 0, closes = 0, listen_stops = 0;
    bool IsAudioChannelOpened() { return opened; }
    bool OpenAudioChannel() { MainOnly(); ++opens; opened = true; return true; }
    void CloseAudioChannel() { MainOnly(); ++closes; opened = false; }
    void SendStopListening() { MainOnly(); ++listen_stops; }
};
struct AudioService { int resets = 0; void ResetDecoder() { MainOnly(); ++resets; } };
struct Application {
    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    int event_group_ = 0;
    DeviceState device_state_ = kDeviceStateIdle;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::unique_ptr<Protocol> protocol_ = std::make_unique<Protocol>();
    AudioService audio_service_;
    int toggles = 0, aborts = 0;
    std::vector<DeviceState> transitions;
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()> callback);
    void SetChatActive(bool active);
    void SetListeningMode(ListeningMode mode);
    void SetDeviceState(DeviceState state) { MainOnly(); transitions.push_back(state); device_state_ = state; }
    void AbortSpeaking(AbortReason) { MainOnly(); ++aborts; }
    void ToggleChatState() { ++toggles; throw std::runtime_error("Menu chat incorrectly routed to ToggleChatState"); }
    void Step() { Require(!main_tasks_.empty(), "Expected queued application callback"); auto cb = std::move(main_tasks_.front()); main_tasks_.pop_front(); cb(); }
    void Drain() { for (int i = 0; !main_tasks_.empty(); ++i) { Require(i < 50, "Unbounded main queue"); Step(); } }
    void Reset() {
        Require(main_tasks_.empty(), "Previous test leaked queued work");
        device_state_ = kDeviceStateIdle; listening_mode_ = kListeningModeAutoStop; aec_mode_ = kAecOff;
        protocol_ = std::make_unique<Protocol>(); audio_service_ = {}; toggles = aborts = 0; transitions.clear();
    }
};
#include "actual_chat_entry.inc"

struct DogMedia {
    bool playing = false, requested = false;
    std::string error, song, response = "service result", alarm_field;
    int toggle = 0, next = 0, prev = 0, stop = 0, alarm = 0, song_reads = 0;
    static DogMedia& GetInstance() { static DogMedia media; return media; }
    std::string TogglePlayback() { MainOnly(); ++toggle; return response; }
    std::string Next() { MainOnly(); ++next; return response; }
    std::string Prev() { MainOnly(); ++prev; return response; }
    std::string Stop() { MainOnly(); ++stop; return response; }
    std::string ToggleAlarm() { MainOnly(); ++alarm; return response; }
    bool IsPlaying() { return playing; }
    bool HasPlaybackRequest() { return requested; }
    std::string LastError() { return error; }
    std::string CurrentSong() { ++song_reads; return song; }
    std::string GetAlarm() { return "alarm summary"; }
    std::string GetAlarmField(const std::string& field) { alarm_field = field; return "alarm " + field; }
    int Actions() { return toggle + next + prev + stop + alarm; }
};
struct Display {
    std::function<void(const std::string&)> action;
    std::function<std::string(const std::string&)> info;
    std::vector<std::string> notifications;
    void SetMenuCallbacks(std::function<void(const std::string&)> a, std::function<std::string(const std::string&)> i) { action = std::move(a); info = std::move(i); }
    void ShowNotification(const char* text) { MainOnly(); notifications.emplace_back(text); }
};
struct Station {
    bool connected = false;
    bool IsConnected() { return connected; }
    std::string GetIpAddress() { return "192.0.2.18"; }
};
Station& GetWifiStation() { static Station station; return station; }
struct SystemInfo { static std::string GetUserAgent() { return "host user agent"; } };
struct AppDescription { const char* version = "test.version"; };
const AppDescription* esp_app_get_description() { static AppDescription desc; return &desc; }
struct BoardDog {
    Display owned_display;
    Display* display_ = &owned_display;
    int reset_requests = 0;
    void RequestNetworkReset() { ++reset_requests; }
    BoardDog() { RegisterMenuCallbacks(); }
    void RegisterMenuCallbacks() {
#include "actual_menu_callbacks.inc"
    }
    void Action(const std::string& name) {
        main_context = false;
        try { owned_display.action(name); } catch (...) { main_context = true; throw; }
        main_context = true;
    }
};

void Reset() { Application::GetInstance().Reset(); DogMedia::GetInstance() = {}; GetWifiStation() = {}; }

void TestServiceRoutes() {
    for (const auto& action : {"music_toggle", "music_next", "music_prev", "music_stop", "alarm_toggle"}) {
        Reset(); BoardDog board;
        auto& app = Application::GetInstance(); auto& media = DogMedia::GetInstance();
        board.Action(action);
        Require(media.Actions() == 0 && board.owned_display.notifications.empty() && app.main_tasks_.size() == 1,
                "Menu service action bypassed main dispatch");
        app.Drain();
        Require(media.Actions() == 1, "Menu action invoked multiple services");
        Require(media.toggle == int(std::string(action) == "music_toggle") && media.next == int(std::string(action) == "music_next") &&
                media.prev == int(std::string(action) == "music_prev") && media.stop == int(std::string(action) == "music_stop") &&
                media.alarm == int(std::string(action) == "alarm_toggle"), "Wrong menu action routing");
        Require(board.owned_display.notifications == std::vector<std::string>{media.response}, "Service response was not shown exactly");
        Require(app.protocol_->opens == 0 && app.transitions.empty() && app.toggles == 0, "Media menu action changed conversation");
        Pass(action);
    }
    for (int state : {0, 1, 2}) {
        Reset(); BoardDog board; auto& media = DogMedia::GetInstance();
        media.playing = state == 2; media.requested = state == 1;
        board.Action("music_toggle"); Application::GetInstance().Drain();
        Require(media.toggle == 1 && media.next == 0 && media.stop == 0,
                "Menu toggle duplicated playback policy instead of calling TogglePlayback");
        Pass("music toggle always delegates stopped/pending/playing policy to media service");
    }
    Reset(); BoardDog board; auto& media = DogMedia::GetInstance();
    board.Action("unrecognized_action"); Application::GetInstance().Drain();
    Require(media.Actions() == 0 && board.owned_display.notifications.empty(), "Unknown action caused effects");
    Pass("unknown menu action has no service effect");
    media.response.clear(); board.Action("music_next"); Application::GetInstance().Drain();
    Require(media.next == 1 && board.owned_display.notifications.empty(), "Empty service response created a notification");
    Pass("empty service response is not shown as a notification");
    media.response = "queued result"; board.Action("music_next"); board.display_ = nullptr;
    Application::GetInstance().Drain();
    Require(media.next == 2 && board.owned_display.notifications.empty(), "Missing display broke queued service action");
    Pass("queued service action tolerates display becoming unavailable");
}

void TestChatEntry() {
    Reset(); BoardDog board; auto& app = Application::GetInstance(); auto& media = DogMedia::GetInstance();
    board.Action("chat");
    Require(media.stop == 0 && app.protocol_->opens == 0, "Chat action bypassed main queue");
    app.Step();
    Require(media.stop == 1 && app.protocol_->opens == 0 && app.main_tasks_.size() == 1,
            "Chat must stop local media before scheduling actual chat entry");
    app.Drain();
    Require(app.device_state_ == kDeviceStateListening && app.protocol_->opens == 1 && app.toggles == 0,
            "Chat menu did not enter through explicit wake");
    Require(board.owned_display.notifications.empty(), "Chat displayed unrelated Stop result");
    Pass("chat stops local media then invokes queued explicit chat entry");

    Reset(); BoardDog duplicate;
    duplicate.Action("chat"); duplicate.Action("chat"); app.Drain();
    Require(app.protocol_->opens == 1 && app.protocol_->closes == 0 && app.transitions.size() == 2 &&
            app.device_state_ == kDeviceStateListening && app.toggles == 0, "Repeated menu chat was not idempotent");
    Pass("two queued chat menu selections enter once and never toggle out");

    for (auto state : {kDeviceStateListening, kDeviceStateSpeaking}) {
        Reset(); BoardDog active; app.device_state_ = state; app.protocol_->opened = true;
        active.Action("chat"); app.Drain();
        Require(app.device_state_ == state && app.protocol_->opens == 0 && app.protocol_->closes == 0 &&
                app.aborts == 0 && app.toggles == 0, "Chat menu interrupted an active conversation");
        Pass("chat menu preserves active listening or speaking state");
    }
    Reset(); BoardDog configuring; app.device_state_ = kDeviceStateWifiConfiguring;
    configuring.Action("chat"); app.Drain();
    Require(app.device_state_ == kDeviceStateWifiConfiguring && app.protocol_->opens == 0 &&
            app.transitions.empty() && app.toggles == 0, "Chat menu entered AudioTesting or bypassed setup guard");
    Pass("actual chat-entry guard preserves Wi-Fi setup without invoking AudioTesting toggle");

    Reset(); BoardDog stale; stale.Action("chat"); app.Step();
    app.device_state_ = kDeviceStateWifiConfiguring; app.Drain();
    Require(app.device_state_ == kDeviceStateWifiConfiguring && app.protocol_->opens == 0 && app.transitions.empty(),
            "Queued chat entry ignored setup state reached after menu dispatch");
    Pass("chat entry checks setup state when nested wake task executes");
}

void TestInformation() {
    Reset(); BoardDog board; auto& media = DogMedia::GetInstance();
    auto info = [&](const std::string& name) { return board.owned_display.info(name); };
    Require(info("music") == "SD卡/music/mp3", "Empty music info advertises incorrect SD directory");
    Pass("empty music info names exact SD card /music/mp3 directory");
    media.song = "测试音乐.mp3"; media.error = "SD卡读取失败"; media.playing = media.requested = true;
    media.song_reads = 0;
    Require(info("music") == media.error && media.song_reads == 0, "Music error was hidden by song/status information");
    Pass("music errors take precedence over selected song and state");
    media.error.clear();
    Require(info("music") == "播放中: 测试音乐.mp3", "Active music label is incorrect");
    Pass("active music reports playing");
    media.playing = false;
    Require(info("music") == "待播放: 测试音乐.mp3", "Pending music incorrectly reported stopped");
    Pass("queued music reports pending before PCM starts");
    media.requested = false;
    Require(info("music") == "已停止: 测试音乐.mp3", "Stopped music label is incorrect");
    Pass("stopped music preserves selected song and reports stopped");
    Require(media.Actions() == 0, "Music info callback changed playback");
    Pass("music information reads never change playback");
    Require(info("alarm") == "alarm summary" && info("alarm_time") == "alarm time" && media.alarm_field == "time",
            "Alarm info callback did not forward field correctly");
    Pass("alarm information forwards summary and exact field key");
    Require(info("about_version") == "vtest.version" && info("about_board") == "zfsn-esp32s3-dog",
            "Board/version information routing changed");
    Pass("about information reads actual version and board fields");
    Require(info("about_network") == "Wi-Fi未连接", "Disconnected Wi-Fi status changed");
    GetWifiStation().connected = true;
    Require(info("about_network") == "192.0.2.18", "Connected Wi-Fi did not report address");
    Pass("network information reports the Wi-Fi state");
    Require(info("other") == "host user agent", "Unknown info page did not preserve fallback");
    Pass("unknown information request preserves system user-agent fallback");
}

int main() {
    try {
        TestServiceRoutes(); TestChatEntry(); TestInformation();
        std::cout << cases << " actual board menu callback tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
