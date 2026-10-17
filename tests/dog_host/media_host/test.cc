#include "stubs/fake.h"
#define private public
#include "dog_media.h"
#undef private

namespace {
int cases = 0;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Pass(const char* message) { ++cases; printf("PASS %s\n", message); }
DogMedia& Media() { return DogMedia::GetInstance(); }

void Reset() {
    host::fail_commit = host::fail_acquire = host::fail_decode = false;
    host::on_delay = host::on_cancel = {};
    Media().SetPlaybackCallback({});
    Media().Stop();
    Media().alarm_ = {};
    Media().alarm_inflight_ = Media().alarm_save_pending_ = false;
    Media().alarm_save_error_.clear();
    Media().last_error_.clear();
    Media().current_path_.clear();
    host::persisted.clear();
    host::pending.clear();
    host::writes = host::begins = host::decoder_opens = host::commits = 0;
    host::micros = 0;
    Application::GetInstance().state = kDeviceStateIdle;
    tm date{}; date.tm_year = 126; date.tm_mon = 9; date.tm_mday = 15;
    date.tm_hour = 12; date.tm_sec = 5; date.tm_isdst = -1;
    host::now = mktime(&date);
}

void Arm() {
    tm date{}; localtime_r(&host::now, &date);
    const auto result = Media().SetAlarm(date.tm_year + 1900, date.tm_mon + 1, date.tm_mday,
                                         date.tm_hour, date.tm_min, "alpha");
    Check(result.find("闹钟已设置") != std::string::npos, "alarm setup failed");
    Check(Media().alarm_.enabled && host::persisted["enabled"] == "1", "alarm was not armed persistently");
}

void WorkerOnce() {
    host::iterations = 1;
    try { Media().Worker(); } catch (const host::EndIteration&) {}
}

void File(const std::filesystem::path& path) {
    FILE* file = fopen(path.string().c_str(), "wb");
    Check(file != nullptr, "cannot create host fixture");
    const uint8_t bytes[] = {0xff, 0xfb, 0x90, 0x00};
    Check(fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes), "cannot write host fixture");
    fclose(file);
}

void OrderedStopToggleRace(bool toggle_first) {
    Reset();
    Media().TogglePlayback();
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    int cancellations = 0;
    host::on_cancel = [&] {
        if (++cancellations != 1) return;
        std::unique_lock lock(mutex);
        entered = true; cv.notify_all();
        cv.wait(lock, [&] { return release; });
    };
    std::thread first([&] { if (toggle_first) Media().TogglePlayback(); else Media().Stop(); });
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return entered; });
    }
    std::atomic<bool> second_entered = false, second_finished = false;
    std::thread second([&] {
        second_entered = true;
        if (toggle_first) Media().Stop(); else Media().TogglePlayback();
        second_finished = true;
    });
    while (!second_entered) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool serialized = !second_finished;
    { std::lock_guard lock(mutex); release = true; }
    cv.notify_all(); first.join(); second.join();
    host::on_cancel = {};
    Check(serialized, "Stop/Toggle command decisions ran concurrently inside audio cancellation");
    Check(Media().HasPlaybackRequest() == !toggle_first && Media().pending_ == !toggle_first,
          "Stop/Toggle final request disagrees with serialized command order");
}
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "expected isolated host SD directory");
        const std::filesystem::path directory(argv[1]);
        Reset();
        Check(Media().Play("alpha").find("无法读取SD卡") != std::string::npos,
              "missing SD directory was reported as an empty playlist");
        Check(Media().LastError().find("无法读取SD卡") != std::string::npos, "SD failure is invisible in status");
        Check(Media().TogglePlayback().find("无法读取SD卡") != std::string::npos && !Media().HasPlaybackRequest(),
              "toggle on missing SD created a stale playback intent");
        std::filesystem::create_directories(directory);
        Check(Media().Next().find("没有可播放") != std::string::npos, "empty SD playlist was not diagnosed");
        Check(Media().LastError().find("没有可播放") != std::string::npos, "empty-playlist error is invisible");
        Check(Media().TogglePlayback().find("没有可播放") != std::string::npos && !Media().HasPlaybackRequest(),
              "toggle on empty SD created a stale playback intent");
        File(directory / "alpha.mp3"); File(directory / "beta.mp3");
        Pass("missing SD directory and empty playlist expose distinct persistent status errors");

        Reset(); Arm();
        Application::GetInstance().state = kDeviceStateSpeaking;
        for (int i = 0; i < 10; ++i) { ++host::now; Media().CheckAlarm(); }
        Check(Media().alarm_.enabled && !Media().pending_ && !Media().alarm_inflight_, "speaking consumed or queued alarm");
        Check(host::persisted["enabled"] == "1", "speaking persisted a false disabled alarm");
        Application::GetInstance().state = kDeviceStateIdle;
        bool started = false;
        Media().SetPlaybackCallback([&](bool playing) {
            if (playing) {
                started = true;
                Check(!Media().alarm_.enabled && host::persisted["enabled"] == "0", "alarm not consumed at first PCM");
            }
        });
        WorkerOnce();
        Check(started && host::writes == 1 && !Media().pending_, "alarm did not play exactly once after speaking");
        Media().CheckAlarm();
        Check(!Media().pending_, "one-shot alarm requeued after playback");
        Pass("10-second TTS defers alarm and first accepted PCM consumes one-shot alarm");

        Reset(); Arm();
        host::fail_acquire = true;
        WorkerOnce();
        Check(Media().alarm_.enabled && !Media().alarm_inflight_ && host::writes == 0,
              "audio acquisition failure consumed alarm or left it stuck in flight");
        Check(!Media().LastError().empty(), "asynchronous acquisition failure is invisible");
        host::fail_acquire = false;
        WorkerOnce();
        Check(host::writes == 1 && !Media().alarm_.enabled, "alarm did not retry after temporary audio contention");
        Pass("audio ownership contention retries without duplicate in-flight requests");

        Reset(); Arm();
        host::fail_decode = true;
        WorkerOnce();
        Check(Media().alarm_.enabled && host::writes == 0 && !Media().alarm_inflight_, "decode failure consumed alarm");
        host::fail_decode = false;
        WorkerOnce();
        Check(host::writes == 1 && !Media().alarm_.enabled, "decode failure was not recoverable within grace period");
        Pass("pre-PCM decoder failure leaves alarm armed for a bounded retry");

        Reset(); Arm();
        Application::GetInstance().state = kDeviceStateSpeaking;
        host::now += 60;
        Media().CheckAlarm();
        Check(!Media().alarm_.enabled && !Media().pending_ && host::writes == 0, "expired alarm unexpectedly played");
        Check(Media().LastError().find("未能播放") != std::string::npos, "missed alarm has no visible explanation");
        Pass("expired busy alarm reports a missed playback instead of claiming success");

        Reset(); Arm(); Media().CheckAlarm();
        Check(Media().alarm_inflight_ && Media().pending_, "due alarm was not queued");
        Media().CheckAlarm();
        const auto generation = Media().generation_.load();
        Media().CheckAlarm();
        Check(Media().generation_.load() == generation, "polling replaced the in-flight alarm");
        Media().Stop();
        Media().CheckAlarm();
        Check(!Media().pending_ && !Media().alarm_.enabled && !Media().IsPlaying(), "stop failed to cancel queued alarm");
        Pass("polling preserves one pending alarm and explicit stop cancels it persistently");

        Reset(); Arm(); Media().CheckAlarm();
        const auto queued_alarm = Media().alarm_generation_;
        Check(Media().Play("beta").find("播放队列") != std::string::npos, "normal replacement did not queue");
        Check(Media().generation_.load() != queued_alarm, "normal request did not replace queued generation");
        Check(!Media().alarm_inflight_, "replaced, unconsumed alarm generation left stale in-flight state");
        WorkerOnce();
        Check(host::writes == 1 && !Media().alarm_.enabled && !Media().alarm_inflight_ &&
              Media().CurrentSong() == "alpha.mp3", "overwritten due alarm did not recover through actual worker");
        Pass("normal request replacing pending alarm clears stale generation and due alarm retries");

        Reset(); Arm();
        host::fail_commit = true;
        WorkerOnce();
        Check(host::writes == 1 && !Media().alarm_.enabled && Media().alarm_save_pending_, "failed persistence lost consumed RAM state");
        Check(host::persisted["enabled"] == "1", "NVS failure stub did not preserve old committed state");
        WorkerOnce();
        Check(host::writes == 1, "NVS failure replayed the alarm");
        host::fail_commit = false;
        Media().CheckAlarm();
        Check(!Media().alarm_save_pending_ && host::persisted["enabled"] == "0", "alarm disable was not retried persistently");
        Pass("NVS failure retries persistence without replaying or truncating consumed alarm");

        Reset();
        std::vector<bool> events;
        Media().SetPlaybackCallback([&](bool playing) {
            events.push_back(playing);
            if (playing) Media().Stop();
        });
        Check(Media().Play("alpha").find("播放队列") != std::string::npos, "normal song did not queue");
        WorkerOnce();
        Check(!Media().IsPlaying() && !Media().pending_ && events == std::vector<bool>({true, false}),
              "stop left stale playing state or duplicated stopped callback");
        Check(Media().Next().find("beta.mp3") != std::string::npos, "stop discarded next-track anchor");
        Check(Media().LastError().empty(), "successful playback request did not clear stale error");
        Pass("stop immediately synchronizes state and callback while retaining next-track anchor");

        Reset();
        Check(Media().GetAlarmField("time") == "未设置" && Media().GetAlarmField("state") == "未设置", "unset alarm fields are ambiguous");
        Arm();
        Check(Media().GetAlarmField("time") == "2026-10-15 12:00" && Media().GetAlarmField("music") == "alpha.mp3" &&
              Media().GetAlarmField("state") == "开启", "compact alarm fields disagree with saved alarm");
        host::fail_commit = true;
        Check(Media().ToggleAlarm().find("保存失败") != std::string::npos && Media().alarm_.enabled,
              "failed user edit mutated the committed in-memory alarm");
        Pass("compact alarm fields reflect snapshot and failed user edit retains prior state");

        Reset();
        Check(Media().TogglePlayback().find("alpha.mp3") != std::string::npos && Media().HasPlaybackRequest() &&
              !Media().IsPlaying(), "first toggle did not queue first song with pending status");
        Check(Media().TogglePlayback().find("已停止") != std::string::npos && !Media().HasPlaybackRequest(),
              "second toggle did not cancel queued request");
        Check(Media().TogglePlayback().find("alpha.mp3") != std::string::npos,
              "toggle after stop skipped the current song");
        Pass("toggle queues first song, cancels queued intent, and replays same current song after stop");

        Reset();
        Application::GetInstance().state = kDeviceStateSpeaking;
        Media().TogglePlayback();
        bool waiting_cancelled = false;
        host::on_delay = [&] {
            if (waiting_cancelled) return;
            waiting_cancelled = true;
            Check(!Media().pending_ && !Media().IsPlaying() && Media().HasPlaybackRequest(),
                  "dequeued TTS wait lost playback intent");
            Check(Media().TogglePlayback().find("已停止") != std::string::npos,
                  "toggle in dequeued TTS wait selected another song instead of stopping");
        };
        WorkerOnce();
        Check(waiting_cancelled && host::writes == 0 && !Media().HasPlaybackRequest() && !Media().pending_,
              "cancelled TTS-wait request reached PCM or remained active");
        Pass("toggle cancels actual worker after dequeue while waiting for TTS, before first PCM");

        Reset();
        bool stopped_playing = false;
        Media().SetPlaybackCallback([&](bool playing) {
            if (!playing) return;
            Check(Media().IsPlaying() && Media().HasPlaybackRequest(), "first PCM did not expose playing intent");
            stopped_playing = Media().TogglePlayback().find("已停止") != std::string::npos;
        });
        Media().TogglePlayback(); WorkerOnce();
        Check(stopped_playing && !Media().IsPlaying() && !Media().HasPlaybackRequest() && !Media().pending_,
              "toggle during actual playback failed to stop");
        Pass("toggle during first accepted PCM stops actual playback and clears intent");

        Reset();
        Application::GetInstance().state = kDeviceStateSpeaking;
        Media().TogglePlayback();
        bool replaced = false;
        host::on_delay = [&] {
            if (replaced) return;
            replaced = true;
            Media().TogglePlayback(); // cancel the dequeued old generation
            Media().TogglePlayback(); // accept a new request while old Worker is unwinding
        };
        WorkerOnce();
        Check(replaced && Media().HasPlaybackRequest() && Media().pending_ && !Media().IsPlaying(),
              "old worker completion cleared newer queued playback intent");
        Media().TogglePlayback();
        Check(!Media().HasPlaybackRequest() && !Media().pending_, "newer pending request could not be cancelled");
        Pass("old worker completion cannot clear newer playback request accepted during TTS wait");

        Reset();
        bool reentered = false;
        Media().SetPlaybackCallback([&](bool playing) {
            if (playing) Media().TogglePlayback();
            else { reentered = true; Media().TogglePlayback(); }
        });
        Media().TogglePlayback(); WorkerOnce();
        Check(reentered && Media().HasPlaybackRequest() && Media().pending_,
              "stopped callback could not reenter toggle or old worker cleared its new intent");
        Pass("stop callback can reenter toggle after command unlock and retain the new request");

        Reset();
        bool continuous = false;
        Media().SetPlaybackCallback([&](bool playing) {
            if (playing) return;
            continuous = Media().HasPlaybackRequest();
            Media().TogglePlayback();
        });
        Media().TogglePlayback(); WorkerOnce();
        Check(continuous && !Media().HasPlaybackRequest() && !Media().pending_,
              "auto-advance exposed a stopped gap or overrode a toggle stop");
        Pass("auto-advance preserves intent through completion callback and toggle cancels continuation");

        Reset();
        File(directory / "removed.mp3");
        Check(Media().Play("removed").find("播放队列") != std::string::npos, "removed fixture did not queue");
        Media().Stop();
        std::filesystem::remove(directory / "removed.mp3");
        Check(Media().TogglePlayback().find("alpha.mp3") != std::string::npos,
              "missing current file did not fall back to first valid song");
        Pass("toggle falls back to first song when remembered current file no longer exists");

        OrderedStopToggleRace(false);
        Pass("concurrent Stop followed by Toggle serializes cancellation before accepting replay");
        OrderedStopToggleRace(true);
        Pass("concurrent Toggle followed by Stop serializes into a fully stopped request state");
        printf("PASS %d media behavior cases\n", cases);
        return 0;
    } catch (const std::exception& error) { fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
