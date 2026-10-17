#ifndef DOG_MEDIA_H
#define DOG_MEDIA_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// SD-card MP3 playback and alarm service.
// AudioService remains the sole writer to the speaker: decoded, resampled PCM
// is submitted through its local-playback API rather than writing I2S here.
class DogMedia {
public:
    static DogMedia& GetInstance();
    bool Start();

    std::string Play(const std::string& song_name, const std::string& artist = "");
    bool PlayFile(const std::string& path, bool loop);
    bool QueueFile(const std::string& path, bool loop, std::string& error);
    std::string Next();
    std::string Prev();
    std::string Stop();
    std::string TogglePlayback();
    std::string Recommend();
    std::string GetAlarm();
    std::string GetAlarmField(const std::string& field);
    std::string ToggleAlarm();
    void ReloadSettings();
    void SetPlaybackCallback(std::function<void(bool)> callback);
    // Zero date fields use today's real, synchronized date. An unset clock is
    // an error; a fabricated calendar date must never be persisted.
    std::string SetAlarm(int year, int month, int day, int hour, int minute,
                         const std::string& song_name, const std::string& artist = "",
                         bool enabled = true);
    std::string CurrentSong();
    std::string LastError();
    bool IsPlaying();
    // Includes queued and dequeued requests waiting for TTS/audio ownership.
    // IsPlaying remains true only after PCM has actually been accepted.
    bool HasPlaybackRequest();

    DogMedia(const DogMedia&) = delete;
    DogMedia& operator=(const DogMedia&) = delete;

private:
    struct Song {
        std::string name;
        std::string path;
    };
    struct Alarm {
        int year = 0;
        int month = 0;
        int day = 0;
        int hour = 0;
        int minute = 0;
        int music_index = -1;
        std::string music_path;
        bool enabled = false;
    };

    DogMedia() = default;
    std::mutex command_mutex_;
    std::mutex mutex_;
    TaskHandle_t task_ = nullptr;
    std::atomic<uint32_t> generation_{0};
    bool decoder_ready_ = false;
    bool pending_ = false;
    bool playing_ = false;
    bool playback_requested_ = false;
    std::string pending_path_;
    int pending_policy_ = 0;  // 0=playlist setting, 1=repeat this file, 2=once, 3=alarm once
    std::string current_path_;
    std::string last_error_;
    Alarm alarm_;
    uint32_t alarm_revision_ = 0;
    uint32_t alarm_generation_ = 0;
    bool alarm_inflight_ = false;
    bool alarm_save_pending_ = false;
    std::string alarm_save_error_;
    int music_mode_ = 1;
    std::function<void(bool)> playback_callback_;

    static std::vector<Song> ScanSongs(std::string* error = nullptr);
    static int FindSong(const std::vector<Song>& songs, const std::string& song_name,
                        const std::string& artist, bool allow_fallback);
    bool SaveAlarm(const Alarm& alarm, std::string& error);
    void LoadAlarm();
    void CheckAlarm();
    bool RequestSong(const Song& song, int policy, std::string& error,
                     uint32_t alarm_revision = 0);
    // Internal command helpers require command_mutex_. Notify playback clients
    // only after that lock is released, so callbacks can issue another command.
    bool RequestSongLocked(const Song& song, int policy, std::string& error,
                           uint32_t alarm_revision = 0);
    std::function<void(bool)> StopLocked();
    std::string ReportError(const std::string& error);
    void DisableAlarmLocked();
    void PlaybackStarted(uint32_t generation);
    std::string QueueSong(const Song& song);
    std::string Step(int direction);
    void Worker();
    bool DecodeFile(const std::string& path, uint32_t generation, bool& completed,
                    std::string& error);
};

#endif
