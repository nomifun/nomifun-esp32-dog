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

#define TAG "Application"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
constexpr int MAIN_EVENT_SCHEDULE = 1;
void xEventGroupSetBits(int, int) {}
namespace Lang { namespace Strings {
constexpr auto STANDBY = "standby";
constexpr auto CONNECTING = "connecting";
constexpr auto LISTENING = "listening";
constexpr auto SPEAKING = "speaking";
} }

namespace {
int tests = 0;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

struct Display {
    int messages_cleared = 0;
    void SetStatus(const char*) {}
    void SetEmotion(const char*) {}
    void SetChatMessage(const char*, const char*) { ++messages_cleared; }
};
struct Led { void OnStateChanged() {} };
struct Board {
    Display display;
    Led led;
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
    Led* GetLed() { return &led; }
    void SetPowerSaveMode(bool) {}
};
struct DeviceStateEventManager {
    static DeviceStateEventManager& GetInstance() { static DeviceStateEventManager manager; return manager; }
    void PostStateChangeEvent(DeviceState, DeviceState) {}
};
struct AudioService {
    int decoder_resets = 0;
    int queued_audio = 3;
    bool voice_processing = false;
    bool wake_word_detection = false;
    bool local_playback = false;
    void ResetDecoder() { ++decoder_resets; queued_audio = 0; }
    bool IsAudioProcessorRunning() { return voice_processing; }
    bool IsAfeWakeWord() { return true; }
    bool IsLocalPlaybackActive() { return local_playback; }
    void EnableVoiceProcessing(bool enabled) { voice_processing = enabled; }
    void EnableWakeWordDetection(bool enabled) { wake_word_detection = enabled; }
};
struct Protocol {
    bool opened = false;
    bool open_succeeds = true;
    int opens = 0;
    int closes = 0;
    int listen_starts = 0;
    int listen_stops = 0;
    int aborts = 0;
    ListeningMode last_mode = kListeningModeManualStop;
    std::function<void()> on_closed;
    bool IsAudioChannelOpened() const { return opened; }
    bool OpenAudioChannel() { ++opens; opened = open_succeeds; return opened; }
    void CloseAudioChannel() {
        ++closes;
        opened = false;
        if (on_closed) on_closed();
    }
    void SendStartListening(ListeningMode mode) { ++listen_starts; last_mode = mode; }
    void SendStopListening() { ++listen_stops; }
    void SendAbortSpeaking(AbortReason) { ++aborts; }
    void OnAudioChannelClosed(std::function<void()> callback) { on_closed = std::move(callback); }
};
struct Application {
    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_ = std::make_unique<Protocol>();
    int event_group_ = 0;
    DeviceState device_state_ = kDeviceStateIdle;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    bool aborted_ = false;
    int clock_ticks_ = 0;
    AudioService audio_service_;
    void SetChatActive(bool active);
    void Schedule(std::function<void()> callback);
    void SetListeningMode(ListeningMode mode);
    void AbortSpeaking(AbortReason reason);
    void SetDeviceState(DeviceState state);
    void RegisterCloseCallback();
    void ReceiveTtsStart();
    void DrainBatch() {
        // MainEventLoop moves one batch out of main_tasks_ before execution.
        auto tasks = std::move(main_tasks_);
        main_tasks_.clear();
        for (auto& task : tasks) task();
    }
    void Drain() {
        int batches = 0;
        while (!main_tasks_.empty()) {
            Require(++batches < 20, "Unexpected task queue loop");
            DrainBatch();
        }
    }
};

#include "actual_application.inc"
#include "actual_close_callback.inc"
#include "actual_tts_start.inc"

namespace {
void TestQueuedDuplicateWake() {
    Application app;
    app.SetChatActive(true);
    app.SetChatActive(true);
    Require(app.device_state_ == kDeviceStateIdle && app.protocol_->opens == 0, "Wake must execute on MainEventLoop");
    app.Drain();
    Require(app.device_state_ == kDeviceStateListening, "Duplicate wake must retain listening");
    Require(app.protocol_->opens == 1 && app.protocol_->listen_starts == 1 && app.protocol_->closes == 0,
            "Queued duplicate wake must open and start listening only once");
    Require(app.protocol_->last_mode == kListeningModeAutoStop, "AEC off must select automatic listening");
    ++tests;
}
void TestActiveWakeAndLifecycleGuards() {
    for (auto state : {kDeviceStateConnecting, kDeviceStateListening, kDeviceStateSpeaking}) {
        Application app;
        app.device_state_ = state;
        app.protocol_->opened = true;
        app.SetChatActive(true);
        app.Drain();
        Require(app.device_state_ == state, "Wake on active session must preserve state");
        Require(app.protocol_->opens == 0 && app.protocol_->closes == 0 && app.protocol_->aborts == 0,
                "Wake on active session must preserve channel and speech");
        ++tests;
    }
    for (auto state : {kDeviceStateUnknown, kDeviceStateStarting, kDeviceStateWifiConfiguring,
                       kDeviceStateUpgrading, kDeviceStateActivating, kDeviceStateAudioTesting, kDeviceStateFatalError}) {
        for (bool active : {false, true}) {
            Application app;
            app.device_state_ = state;
            app.protocol_->opened = true;
            app.SetChatActive(active);
            app.Drain();
            Require(app.device_state_ == state && app.protocol_->opens == 0 && app.protocol_->closes == 0,
                    "Chat request must not interrupt application lifecycle");
            Require(app.audio_service_.decoder_resets == 0 && app.protocol_->aborts == 0 && app.protocol_->listen_stops == 0,
                    "Ignored lifecycle request must preserve audio and protocol");
            ++tests;
        }
    }
}
void TestExecutionTimeState() {
    Application app;
    app.SetChatActive(true);
    app.device_state_ = kDeviceStateUpgrading;
    app.Drain();
    Require(app.device_state_ == kDeviceStateUpgrading && app.protocol_->opens == 0,
            "Queued wake must use execution state instead of request state");
    app.device_state_ = kDeviceStateSpeaking;
    app.SetChatActive(false);
    app.device_state_ = kDeviceStateWifiConfiguring;
    app.Drain();
    Require(app.device_state_ == kDeviceStateWifiConfiguring && app.audio_service_.decoder_resets == 0,
            "Queued sleep must not override newer setup state");
    ++tests;
}
void TestOpenModesAndFailure() {
    for (auto mode : {kAecOnDeviceSide, kAecOnServerSide}) {
        Application app;
        app.aec_mode_ = mode;
        app.SetChatActive(true);
        app.Drain();
        Require(app.protocol_->last_mode == kListeningModeRealtime && app.device_state_ == kDeviceStateListening,
                "AEC enabled must select real-time listening");
        ++tests;
    }
    Application existing;
    existing.protocol_->opened = true;
    existing.SetChatActive(true);
    existing.Drain();
    Require(existing.protocol_->opens == 0 && existing.protocol_->listen_starts == 1,
            "Idle with open channel must reuse channel and start listening");
    ++tests;

    Application app;
    app.RegisterCloseCallback();
    app.protocol_->open_succeeds = false;
    app.SetChatActive(true);
    app.Drain();
    Require(app.device_state_ == kDeviceStateIdle && app.protocol_->opens == 1 && app.protocol_->closes == 1,
            "Failed open must release resources and return to Idle");
    Require(app.protocol_->listen_starts == 0, "Failed open must not start listening");
    app.protocol_->open_succeeds = true;
    app.SetChatActive(true);
    app.Drain();
    Require(app.device_state_ == kDeviceStateListening && app.protocol_->opens == 2,
            "Wake must be retryable after channel failure");
    ++tests;
}
void TestSleepListeningAndSpeaking() {
    for (auto state : {kDeviceStateListening, kDeviceStateSpeaking, kDeviceStateConnecting}) {
        Application app;
        app.RegisterCloseCallback();
        app.device_state_ = state;
        app.protocol_->opened = true;
        app.audio_service_.voice_processing = true;
        app.SetChatActive(false);
        app.SetChatActive(false);
        app.Drain();
        Require(app.device_state_ == kDeviceStateIdle && !app.protocol_->opened && !app.audio_service_.voice_processing,
                "Sleep must stop voice processing and close the session");
        Require(app.protocol_->closes == 1 && app.audio_service_.decoder_resets >= 1 && app.audio_service_.queued_audio == 0,
                "Duplicate sleep must clear playback and close only once");
        Require(app.protocol_->listen_stops == (state == kDeviceStateListening ? 1 : 0),
                "Sleep must send stop listening while state is still Listening");
        Require(app.protocol_->aborts == (state == kDeviceStateSpeaking ? 1 : 0),
                "Sleep while speaking must abort the cloud utterance");
        ++tests;
    }
    Application idle;
    idle.SetChatActive(false);
    idle.Drain();
    Require(idle.protocol_->closes == 0 && idle.device_state_ == kDeviceStateIdle &&
            idle.audio_service_.decoder_resets == 1 && idle.audio_service_.queued_audio == 0,
            "Sleep in Idle with closed channel must flush residual PCM without changing state or channel");
    ++tests;
    idle.protocol_->opened = true;
    idle.SetChatActive(false);
    idle.Drain();
    Require(idle.protocol_->closes == 1 && idle.audio_service_.decoder_resets == 2,
            "Sleep in Idle with a live channel must release it");
    ++tests;
}
void TestUnavailableProtocol() {
    Application app;
    app.protocol_.reset();
    app.SetChatActive(true);
    app.Drain();
    Require(app.device_state_ == kDeviceStateIdle, "Unavailable protocol wake must remain Idle");
    for (auto state : {kDeviceStateListening, kDeviceStateSpeaking, kDeviceStateConnecting}) {
        app.device_state_ = state;
        app.audio_service_.queued_audio = 3;
        app.SetChatActive(false);
        app.Drain();
        Require(app.device_state_ == kDeviceStateIdle && app.audio_service_.queued_audio == 0,
                "Sleep must recover active state safely without protocol");
        ++tests;
    }
    ++tests;
}
void TestQueuedSleepWakeAndCloseGuards() {
    Application app;
    app.RegisterCloseCallback();
    app.device_state_ = kDeviceStateListening;
    app.protocol_->opened = true;
    app.audio_service_.voice_processing = true;
    app.SetChatActive(false);
    app.SetChatActive(true);
    app.DrainBatch();
    Require(app.device_state_ == kDeviceStateListening && app.protocol_->opened,
            "Sleep then wake must establish a new listening session");
    Require(!app.main_tasks_.empty(), "Actual close callback must be queued for next batch");
    app.Drain();
    Require(app.device_state_ == kDeviceStateListening && app.protocol_->opened,
            "Old close callback must not idle a reopened session");
    Require(app.protocol_->listen_stops == 1 && app.protocol_->listen_starts == 1,
            "Sleep then wake must send stop and start exactly once");
    ++tests;

    Application failing;
    failing.RegisterCloseCallback();
    failing.protocol_->open_succeeds = false;
    failing.SetChatActive(true);
    failing.DrainBatch();
    failing.protocol_->open_succeeds = true;
    failing.SetChatActive(true);
    failing.Drain();
    Require(failing.device_state_ == kDeviceStateListening, "Failure close notification must not break subsequent wake");
    ++tests;

    for (auto state : {kDeviceStateWifiConfiguring, kDeviceStateUpgrading, kDeviceStateActivating}) {
        Application guard;
        guard.RegisterCloseCallback();
        guard.protocol_->CloseAudioChannel();
        guard.device_state_ = state;
        guard.Drain();
        Require(guard.device_state_ == state, "Late close callback must preserve lifecycle state");
        ++tests;
    }
    Application closed;
    closed.RegisterCloseCallback();
    closed.device_state_ = kDeviceStateSpeaking;
    closed.protocol_->CloseAudioChannel();
    closed.Drain();
    Require(closed.device_state_ == kDeviceStateIdle, "Closed active session must still return to standby");
    ++tests;
}
void TestTtsAfterSleepAndNormalTts() {
    for (bool callback_before_sleep_executes : {false, true}) {
        Application app;
        app.RegisterCloseCallback();
        app.device_state_ = kDeviceStateListening;
        app.protocol_->opened = true;
        app.SetChatActive(false);
        if (!callback_before_sleep_executes) app.Drain();
        app.ReceiveTtsStart();
        app.Drain();
        Require(app.device_state_ == kDeviceStateIdle && !app.protocol_->opened,
                "Queued or late TTS start after sleep must not revive speaking");
        ++tests;
    }
    for (auto state : {kDeviceStateIdle, kDeviceStateListening}) {
        Application app;
        app.device_state_ = state;
        app.protocol_->opened = true;
        app.aborted_ = true;
        app.ReceiveTtsStart();
        app.Drain();
        Require(app.device_state_ == kDeviceStateSpeaking && !app.aborted_,
                "Normal TTS start on an open channel must enter speaking and clear previous abort");
        ++tests;
    }
    Application music;
    music.protocol_->opened = true;
    music.audio_service_.local_playback = true;
    music.aborted_ = true;
    music.ReceiveTtsStart();
    music.Drain();
    Require(music.device_state_ == kDeviceStateIdle && music.aborted_ && music.audio_service_.decoder_resets == 0,
            "Cloud TTS must preserve local playback guard and audio");
    ++tests;

    Application unavailable;
    unavailable.protocol_.reset();
    unavailable.aborted_ = true;
    unavailable.ReceiveTtsStart();
    unavailable.Drain();
    Require(unavailable.device_state_ == kDeviceStateIdle && unavailable.aborted_,
            "TTS start without protocol must be safe and preserve abort state");
    ++tests;
}
}

int main() {
    try {
        TestQueuedDuplicateWake();
        TestActiveWakeAndLifecycleGuards();
        TestExecutionTimeState();
        TestOpenModesAndFailure();
        TestSleepListeningAndSpeaking();
        TestUnavailableProtocol();
        TestQueuedSleepWakeAndCloseGuards();
        TestTtsAfterSleepAndNormalTts();
        std::cout << tests << " actual-function chat-control behavior tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
