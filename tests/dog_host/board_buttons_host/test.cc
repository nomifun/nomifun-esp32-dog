#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

enum class Context { Main, Timer };
Context context = Context::Main;
int cases;
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void MainOnly() { Require(context == Context::Main, "UI or volume persistence executed in timer context"); }
void Pass(const char* message) { ++cases; std::cout << "PASS " << message << '\n'; }

class Application {
public:
    std::deque<std::function<void()>> queue;
    int toggles = 0;
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()> callback) { queue.push_back(std::move(callback)); }
    void ToggleChatState() { MainOnly(); ++toggles; }
    void Step() {
        Require(context == Context::Main, "Main queue must run in main context");
        Require(!queue.empty(), "Expected queued application work");
        auto callback = std::move(queue.front()); queue.pop_front(); callback();
    }
    void Drain() { for (int i = 0; !queue.empty(); ++i) { Require(i < 100, "Unbounded callback queue"); Step(); } }
};

struct Timer {
    void (*callback)(void*) = nullptr;
    void* arg = nullptr;
    bool active = false;
    bool periodic = false;
    int starts = 0, stops = 0;
    long long delay = 0;
};
using esp_timer_handle_t = Timer*;
struct esp_timer_create_args_t {
    void (*callback)(void*);
    void* arg;
    int dispatch_method;
    const char* name;
    bool skip_unhandled_events;
};
constexpr int ESP_TIMER_TASK = 1;
int esp_timer_create(const esp_timer_create_args_t* args, Timer** target) {
    *target = new Timer{args->callback, args->arg}; return 0;
}
int esp_timer_start_periodic(Timer* timer, long long delay) {
    timer->active = true; timer->periodic = true; timer->delay = delay; ++timer->starts; return 0;
}
int esp_timer_start_once(Timer* timer, long long delay) {
    timer->active = true; timer->periodic = false; timer->delay = delay; ++timer->starts; return 0;
}
int esp_timer_stop(Timer* timer) {
    Require(timer != nullptr, "Timer must exist"); timer->active = false; ++timer->stops; return 0;
}
void TimerCall(const std::function<void()>& callback) {
    Require(context == Context::Main, "Nested timer test call");
    context = Context::Timer;
    try { callback(); } catch (...) { context = Context::Main; throw; }
    context = Context::Main;
}
void Fire(Timer* timer) {
    Require(timer->active, "Expected active timer");
    if (!timer->periodic) timer->active = false;
    TimerCall([=]() { timer->callback(timer->arg); });
}
#define ESP_ERROR_CHECK(value) Require((value) == 0, "ESP operation failed")
#define ESP_LOGI(...) ((void)0)
constexpr int BUTTON_1_GPIO = 1;
constexpr int DEFAULT_STEPS = 2;
constexpr int DEFAULT_TURN_ANGLE = 20;
int button1_level = 1;
int gpio_get_level(int) { return button1_level; }

struct Button {
    std::function<void()> click, long_press, press_down, press_up, double_click;
    void OnClick(std::function<void()> cb) { click = std::move(cb); }
    void OnLongPress(std::function<void()> cb) { long_press = std::move(cb); }
    void OnPressDown(std::function<void()> cb) { press_down = std::move(cb); }
    void OnPressUp(std::function<void()> cb) { press_up = std::move(cb); }
    void OnDoubleClick(std::function<void()> cb) { double_click = std::move(cb); }
};
struct ButtonManager { Button boot_button, button1, button2, button3; };
struct Sh1106Display {
    bool menu = false, volume = false;
    int next = 0, previous = 0, confirm = 0, back = 0, exits = 0;
    bool MenuActive() { MainOnly(); return menu; }
    bool VolumePageActive() { MainOnly(); return menu && volume; }
    void MenuNext() { MainOnly(); ++next; menu = true; }
    void MenuPrevious() { MainOnly(); ++previous; menu = true; }
    void MenuConfirm() { MainOnly(); ++confirm; menu = true; }
    void MenuBack() { MainOnly(); ++back; }
    void ExitMenu() { MainOnly(); ++exits; menu = volume = false; }
    void ShowNotification(const char*) { MainOnly(); }
};
struct Codec {
    int value = 50, writes = 0;
    int output_volume() { MainOnly(); return value; }
    void SetOutputVolume(int volume) { MainOnly(); value = volume; ++writes; }
};
struct DogMedia {
    bool playing = false, pending = false;
    int next = 0, stops = 0;
    static DogMedia& GetInstance() { static DogMedia media; return media; }
    bool IsPlaying() { MainOnly(); return playing; }
    bool HasPlaybackRequest() { MainOnly(); return playing || pending; }
    void Stop() { MainOnly(); playing = pending = false; ++stops; }
    void Next() { MainOnly(); ++next; }
};
struct PetDog {
    int actions = 0;
    bool DoAction(const char*, int, int) { MainOnly(); ++actions; return true; }
};

class BoardDog {
public:
    Sh1106Display owned_display;
    Codec owned_codec;
    Sh1106Display* display_ = &owned_display;
    Codec* codec_ = &owned_codec;
    ButtonManager* buttons_ = nullptr;
    PetDog pet_dog_;
    Timer* volume_inc_timer_ = nullptr;
    Timer* volume_dec_timer_ = nullptr;
    Timer* music_switch_timer_ = nullptr;
    std::atomic<bool> volume_inc_held_{false}, volume_dec_held_{false};
    int resets = 0;
    Codec* GetAudioCodec() { MainOnly(); return codec_; }
    Sh1106Display* GetDisplay() { return display_; }
    void RequestNetworkReset() { ++resets; }
    BoardDog() { InitializeVolumeTimers(); InitializeButtons(); }
    ~BoardDog() { delete buttons_; delete volume_inc_timer_; delete volume_dec_timer_; delete music_switch_timer_; }
#include "actual_board_buttons.inc"
};

void ResetGlobals() {
    Require(Application::GetInstance().queue.empty(), "Test leaked queued work");
    Application::GetInstance().toggles = 0;
    DogMedia::GetInstance() = {};
    button1_level = 1;
}

void TestMenuDispatch() {
    ResetGlobals(); BoardDog board;
    auto& app = Application::GetInstance();
    const std::vector<std::function<void()>> callbacks = {
        board.buttons_->button1.click, board.buttons_->button3.click,
        board.buttons_->button2.click, board.buttons_->button2.long_press,
    };
    for (const auto& callback : callbacks) TimerCall(callback);
    Require(board.owned_display.next + board.owned_display.previous + board.owned_display.confirm + board.owned_display.back == 0,
            "Navigation happened before application dispatch");
    Require(app.queue.size() == callbacks.size(), "Every navigation event must be queued");
    app.Drain();
    Require(board.owned_display.next == 1 && board.owned_display.previous == 1 &&
            board.owned_display.confirm == 1 && board.owned_display.back == 1, "Navigation mapping changed");
    Pass("all four menu navigation actions queue UI work outside timer callbacks");
}

void TestVolume() {
    for (int direction : {1, -1}) {
        ResetGlobals(); BoardDog board;
        auto& app = Application::GetInstance();
        Button& button = direction > 0 ? board.buttons_->button1 : board.buttons_->button3;
        Timer* timer = direction > 0 ? board.volume_inc_timer_ : board.volume_dec_timer_;
        auto& display = board.owned_display;
        auto& codec = board.owned_codec;
        display.menu = display.volume = true;

        TimerCall(button.press_down); TimerCall(button.long_press); TimerCall(button.press_up);
        app.Drain();
        Require(timer->starts == 0 && codec.writes == 0, "Released queued long press started repetition");
        Pass("released long press is ignored before queued activation");

        TimerCall(button.press_down); TimerCall(button.long_press);
        app.Step();
        Require(timer->active && timer->delay == 150000 && codec.writes == 0, "Initial volume write bypassed application queue");
        TimerCall(button.press_up); app.Drain();
        Require(!timer->active && codec.writes == 0, "Nested initial volume callback survived release");
        Pass("release between activation and nested volume work cancels both write and repetition");

        TimerCall(button.press_down); TimerCall(button.long_press); app.Drain();
        Require(codec.value == 50 + direction * 2 && timer->active, "Held volume key did not adjust volume");
        int writes = codec.writes;
        Fire(timer);
        Require(codec.writes == writes, "Repeat timer directly performed volume persistence");
        TimerCall(button.press_up); app.Drain();
        Require(codec.writes == writes && !timer->active, "Queued repeat survived key release");
        Pass("timer repeats queue persistence and discard work after release");

        TimerCall(button.press_down); TimerCall(button.long_press); app.Drain();
        writes = codec.writes;
        Fire(timer); display.volume = false; app.Drain();
        Require(codec.writes == writes && !timer->active, "Queued repeat survived leaving volume page");
        TimerCall(button.press_up);
        Pass("queued repeat after leaving volume page stops timer without changing volume");

        display.menu = true; display.volume = false;
        const int starts = timer->starts;
        TimerCall(button.press_down); TimerCall(button.long_press); app.Drain();
        Require(timer->starts == starts && codec.writes == writes, "Non-volume menu started volume adjustment");
        TimerCall(button.press_up);
        Pass("long press outside volume page does not start repeat");

        display.volume = true; codec.value = direction > 0 ? 99 : 1;
        TimerCall(button.press_down); TimerCall(button.long_press); app.Drain();
        Require(codec.value == (direction > 0 ? 100 : 0), "Volume endpoint was not clamped");
        Fire(timer); app.Drain();
        Require(codec.value == (direction > 0 ? 100 : 0), "Repeat exceeded volume endpoint");
        TimerCall(button.press_up);
        Pass("held volume and repeats clamp to the 0..100 range");
    }
}

void TestMediaAndBoot() {
    ResetGlobals(); BoardDog board;
    auto& app = Application::GetInstance();
    auto& media = DogMedia::GetInstance();
    media.playing = true; board.owned_display.menu = true;
    TimerCall(board.buttons_->button2.double_click);
    TimerCall(board.buttons_->button3.double_click); app.Drain();
    Require(media.next == 0 && !board.music_switch_timer_->active, "Active menu double click switched music");
    Pass("both double clicks suppress music switching while menu is active");

    board.owned_display.menu = false;
    TimerCall(board.buttons_->button2.double_click);
    TimerCall(board.buttons_->button3.double_click);
    board.owned_display.menu = true; app.Drain();
    Require(media.next == 0 && !board.music_switch_timer_->active, "Queued double click ignored newly opened menu");
    Pass("queued double clicks recheck active menu before changing tracks");

    board.owned_display.menu = false;
    TimerCall(board.buttons_->button2.double_click);
    Require(media.next == 0, "Double click media work bypassed queue"); app.Drain();
    Require(media.next == 1, "Button2 double click did not switch playing music");
    TimerCall(board.buttons_->button3.double_click); app.Drain();
    Require(board.music_switch_timer_->active && board.music_switch_timer_->delay == 200000,
            "Button3 double click did not preserve delayed switch");
    Fire(board.music_switch_timer_);
    Require(media.next == 1, "Music timer performed playback directly"); app.Drain();
    Require(media.next == 2, "Music timer did not dispatch track switch");
    Pass("ordinary double click and delayed music switch dispatch through application queue");

    TimerCall(board.buttons_->button3.double_click); app.Drain();
    board.owned_display.menu = true;
    Fire(board.music_switch_timer_); app.Drain();
    Require(media.next == 2 && !board.music_switch_timer_->active,
            "Delayed switch ignored menu opened before timer fire");
    Pass("delayed music switch cancels when menu opens before timer fires");

    board.owned_display.menu = false;
    TimerCall(board.buttons_->button3.double_click); app.Drain();
    Fire(board.music_switch_timer_);
    board.owned_display.menu = true; app.Drain();
    Require(media.next == 2, "Queued timer work ignored newly opened menu");
    Pass("delayed music switch rechecks menu again when queued timer work executes");

    for (bool stop_before_fire : {true, false}) {
        board.owned_display.menu = false; media.playing = true;
        TimerCall(board.buttons_->button3.double_click); app.Drain();
        if (stop_before_fire) media.playing = false;
        Fire(board.music_switch_timer_);
        media.playing = false; app.Drain();
        Require(media.next == 2, "Delayed switch advanced stopped media");
    }
    media.playing = true;
    Pass("delayed music switch cancels if media stops before timer or queued work");

    board.owned_display.menu = true; button1_level = 0;
    TimerCall(board.buttons_->button3.double_click);
    Require(board.pet_dog_.actions == 0, "Servo test bypassed queue");
    button1_level = 1; app.Drain();
    Require(board.pet_dog_.actions == 1 && media.next == 2, "Two-key servo test lost captured key state");
    Pass("two-key servo test captures gesture and queues motion");

    board.owned_display.menu = true; board.owned_display.volume = true;
    TimerCall(board.buttons_->boot_button.click);
    Require(board.owned_display.exits == 0 && media.stops == 0, "BOOT click bypassed queue"); app.Drain();
    Require(board.owned_display.exits == 1 && !board.owned_display.menu && !media.playing &&
            media.stops == 1 && app.toggles == 0, "BOOT failed to exit menu and stop music");
    Pass("BOOT exits menu before stopping active music");
    board.owned_display.menu = board.owned_display.volume = true;
    media.pending = true;
    Require(!media.playing, "Pending BOOT test must run before PCM playback");
    TimerCall(board.buttons_->boot_button.click);
    Require(board.owned_display.exits == 1 && media.pending && media.stops == 1 && app.toggles == 0,
            "Pending BOOT action bypassed application queue");
    app.Drain();
    Require(board.owned_display.exits == 2 && !board.owned_display.menu && !board.owned_display.volume &&
            !media.playing && !media.pending && media.stops == 2 && app.toggles == 0,
            "BOOT failed to exit menu and cancel pending playback without toggling chat");
    Pass("BOOT exits menu and stops pending playback without opening conversation");
    board.owned_display.menu = true;
    TimerCall(board.buttons_->boot_button.click); app.Drain();
    Require(board.owned_display.exits == 3 && !board.owned_display.menu && app.toggles == 1,
            "BOOT failed to exit menu and toggle conversation");
    Pass("BOOT exits menu before toggling conversation");

    board.display_ = nullptr;
    TimerCall(board.buttons_->button1.click); TimerCall(board.buttons_->button3.click);
    TimerCall(board.buttons_->button2.click); TimerCall(board.buttons_->button2.long_press);
    TimerCall(board.buttons_->boot_button.click); app.Drain();
    Require(app.toggles == 2, "Display failure prevented conversation button");
    TimerCall(board.buttons_->button1.press_down); TimerCall(board.buttons_->button1.long_press); app.Drain();
    Require(!board.volume_inc_timer_->active, "Missing display started volume repetition");
    TimerCall(board.buttons_->button1.press_up);
    Pass("missing display tolerates navigation and preserves BOOT conversation control");
}

int main() {
    try {
        TestMenuDispatch(); TestVolume(); TestMediaAndBoot();
        std::cout << cases << " actual board button/volume behavior tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
