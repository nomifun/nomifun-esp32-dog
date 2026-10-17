#include "dog_media.h"

#include "application.h"
#include "board.h"
#include "settings.h"

#include <esp_audio_simple_dec.h>
#include <esp_log.h>
#include <esp_mp3_dec.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <nvs.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>

namespace {
constexpr const char* kTag = "MP3Player";
constexpr const char* kMusicDirectory = "/sdcard/music/mp3";

std::string FileName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string SearchKey(const std::string& value) {
    std::string key;
    key.reserve(value.size());
    for (unsigned char c : value) {
        // Keep UTF-8 bytes intact. The SD filename contract includes Chinese
        // names; ASCII case / whitespace / punctuation should not defeat them.
        if (c >= 0x80 || std::isalnum(c)) {
            key.push_back(c >= 0x80 ? static_cast<char>(c) : static_cast<char>(std::tolower(c)));
        }
    }
    return key;
}

bool HasMp3Extension(const std::string& name) {
    return name.size() > 4 && SearchKey(name.substr(name.size() - 4)) == "mp3";
}

bool ValidClock(std::tm& now_tm) {
    const time_t now = time(nullptr);
    return localtime_r(&now, &now_tm) != nullptr && now_tm.tm_year + 1900 >= 2025 &&
           now_tm.tm_year + 1900 <= 2099;
}

bool ValidDate(int year, int month, int day, int hour, int minute, time_t& timestamp) {
    std::tm date = {};
    date.tm_year = year - 1900;
    date.tm_mon = month - 1;
    date.tm_mday = day;
    date.tm_hour = hour;
    date.tm_min = minute;
    date.tm_isdst = -1;
    timestamp = mktime(&date);
    return timestamp != static_cast<time_t>(-1) && date.tm_year == year - 1900 &&
           date.tm_mon == month - 1 && date.tm_mday == day &&
           date.tm_hour == hour && date.tm_min == minute;
}

std::string Calendar(int year, int month, int day, int hour, int minute) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d", year, month, day, hour, minute);
    return buffer;
}

// Persistent linear interpolation supports MP3's 44.1 kHz family as well as
// 8/16/24/32/48 kHz. The existing SILK speech resampler rejects 44.1 kHz.
// Positions use integer ratios, preserving continuity between decoded frames.
class MusicResampler {
public:
    std::vector<int16_t> Process(const int16_t* input, size_t frames, int channels,
                                  int input_rate, int output_rate) {
        if (input_rate_ != input_rate || output_rate_ != output_rate) {
            input_rate_ = input_rate;
            output_rate_ = output_rate;
            total_frames_ = 0;
            next_position_ = 0;
        }
        std::vector<int16_t> mono(frames);
        for (size_t i = 0; i < frames; ++i) {
            int32_t sample = input[i * channels];
            if (channels == 2) {
                sample = (sample + input[i * channels + 1]) / 2;
            }
            mono[i] = static_cast<int16_t>(sample);
        }
        std::vector<int16_t> output;
        if (frames == 0) {
            return output;
        }
        output.reserve(frames * output_rate / input_rate + 2);
        const int64_t end = (total_frames_ + frames - 1) * output_rate;
        while (next_position_ < end) {
            const int64_t source = next_position_ / output_rate;
            const int64_t offset = source - total_frames_;
            const int32_t first = offset < 0 ? previous_ : mono[static_cast<size_t>(offset)];
            const int32_t second = mono[static_cast<size_t>(offset + 1)];
            const int64_t fraction = next_position_ % output_rate;
            output.push_back(static_cast<int16_t>(first +
                (second - first) * fraction / output_rate));
            next_position_ += input_rate;
        }
        previous_ = mono.back();
        total_frames_ += frames;
        return output;
    }

private:
    int input_rate_ = 0;
    int output_rate_ = 0;
    int64_t total_frames_ = 0;
    int64_t next_position_ = 0;
    int16_t previous_ = 0;
};
}  // namespace

DogMedia& DogMedia::GetInstance() {
    static DogMedia media;
    return media;
}

bool DogMedia::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_ != nullptr) {
        return true;
    }
    const auto result = esp_mp3_dec_register();
    decoder_ready_ = result == ESP_AUDIO_ERR_OK || result == ESP_AUDIO_ERR_ALREADY_EXIST;
    if (!decoder_ready_) {
        last_error_ = "MP3解码器初始化失败: " + std::to_string(result);
        ESP_LOGE(kTag, "%s", last_error_.c_str());
        return false;
    }
    LoadAlarm();
    Settings settings("display");
    music_mode_ = settings.GetInt("music_mode", 1);
    if (music_mode_ < 0 || music_mode_ > 2) music_mode_ = 1;
    if (xTaskCreate([](void* arg) {
        static_cast<DogMedia*>(arg)->Worker();
        vTaskDelete(nullptr);
    }, "mp3_async_play", 10240, this, 3, &task_) != pdPASS) {
        task_ = nullptr;
        last_error_ = "无法创建音乐/闹钟任务";
        return false;
    }
    ESP_LOGI(kTag, "MP3 player initialized successfully");
    return true;
}

std::vector<DogMedia::Song> DogMedia::ScanSongs(std::string* error) {
    std::vector<Song> songs;
    DIR* directory = opendir(kMusicDirectory);
    if (directory == nullptr) {
        if (error) *error = "无法读取SD卡 music/mp3 目录，请检查SD卡和目录后重试";
        return songs;
    }
    while (auto* entry = readdir(directory)) {
        std::string name(entry->d_name);
        if (name.empty() || name.front() == '.' || !HasMp3Extension(name)) {
            continue;
        }
        const std::string path = std::string(kMusicDirectory) + "/" + name;
        struct stat info = {};
        if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0) {
            songs.push_back({std::move(name), path});
        }
    }
    closedir(directory);
    std::sort(songs.begin(), songs.end(), [](const Song& a, const Song& b) {
        return a.name < b.name;
    });
    if (error) *error = songs.empty() ? "SD卡 music/mp3 目录中没有可播放的MP3文件" : "";
    return songs;
}

int DogMedia::FindSong(const std::vector<Song>& songs, const std::string& song_name,
                      const std::string& artist, bool allow_fallback) {
    const std::string song_key = SearchKey(song_name);
    const std::string artist_key = SearchKey(artist);
    if (song_key.empty() && artist_key.empty()) {
        return -1;
    }
    auto find = [&](bool use_song, bool use_artist) {
        int best = -1;
        int best_score = -1;
        for (size_t i = 0; i < songs.size(); ++i) {
            const std::string stem = songs[i].name.substr(0, songs[i].name.size() - 4);
            const std::string key = SearchKey(stem);
            if ((use_song && key.find(song_key) == std::string::npos) ||
                (use_artist && key.find(artist_key) == std::string::npos)) {
                continue;
            }
            auto separator = stem.find(" - ");
            if (separator == std::string::npos) {
                separator = stem.find('-');
            }
            const std::string title = SearchKey(stem.substr(0, separator));
            int score = (use_song && title == song_key ? 1000 : 0) +
                        (use_song && key == song_key ? 500 : 0) +
                        (use_artist ? 100 : 0);
            if (score > best_score) {
                best = static_cast<int>(i);
                best_score = score;
            }
        }
        return best;
    };
    int result = find(!song_key.empty(), !artist_key.empty());
    if (result < 0 && allow_fallback && !song_key.empty() && !artist_key.empty()) {
        ESP_LOGW(kTag, "song+artist match failed, retry song only: '%s'", song_name.c_str());
        result = find(true, false);
        if (result < 0) {
            ESP_LOGW(kTag, "song-only match failed, retry artist only: '%s'", artist.c_str());
            result = find(false, true);
        }
    }
    return result;
}

bool DogMedia::RequestSong(const Song& song, int policy, std::string& error,
                           uint32_t alarm_revision) {
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    return RequestSongLocked(song, policy, error, alarm_revision);
}

bool DogMedia::RequestSongLocked(const Song& song, int policy, std::string& error,
                                 uint32_t alarm_revision) {
    if (!Start()) {
        error = LastError();
        return false;
    }
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(song.path.c_str(), "rb"), &fclose);
    if (!file) {
        error = "无法打开 " + song.name;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (policy == 3 && (!alarm_.enabled || alarm_inflight_ || alarm_revision_ != alarm_revision)) {
            return false;
        }
        // The pending slot can be replaced before Worker ever takes the old
        // alarm generation. Its completion path then never runs; release that
        // marker here so the still-armed alarm can retry on the next poll.
        if (policy != 3) alarm_inflight_ = false;
        pending_path_ = song.path;
        pending_policy_ = policy;
        pending_ = true;
        playback_requested_ = true;
        current_path_ = song.path;
        last_error_.clear();
        ++generation_;
        if (policy == 3) {
            alarm_inflight_ = true;
            alarm_generation_ = generation_.load();
        }
        xTaskNotifyGive(task_);
    }
    return true;
}

std::string DogMedia::QueueSong(const Song& song) {
    std::string error;
    if (!RequestSong(song, 0, error)) {
        return ReportError("播放失败: " + error);
    }
    // Playback is asynchronous.
    // Say queued until the decoder has actually acquired the audio service.
    return "本地歌曲已加入播放队列：" + song.name;
}

bool DogMedia::PlayFile(const std::string& file, bool loop) {
    std::string error;
    return QueueFile(file, loop, error);
}

bool DogMedia::QueueFile(const std::string& file, bool loop, std::string& error) {
    error.clear();
    std::string path;
    if (file.compare(0, 8, "/sdcard/") == 0) {
        path = file;
    } else if (file.compare(0, 7, "sdcard/") == 0) {
        path = "/" + file;
    } else if (file.compare(0, 6, "music/") == 0) {
        path = "/sdcard/" + file;
    } else if (file.compare(0, 7, "/music/") == 0) {
        path = "/sdcard" + file;
    } else if (file.find('/') == std::string::npos) {
        path = std::string(kMusicDirectory) + "/" + file;
    }
    const bool unsafe = path.empty() || path.find("/../") != std::string::npos ||
                        path.find("/./") != std::string::npos || path.find('\\') != std::string::npos ||
                        path.find('\0') != std::string::npos || !HasMp3Extension(path);
    struct stat info = {};
    if (unsafe || stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) {
        error = "音乐路径无效或文件不存在: " + file;
    } else if (RequestSong({FileName(path), path}, loop ? 1 : 2, error)) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = error;
    return false;
}

std::string DogMedia::Play(const std::string& song_name, const std::string& artist) {
    if (SearchKey(song_name).empty() && SearchKey(artist).empty()) {
        return ReportError("请至少提供歌名或歌手关键词");
    }
    std::string error;
    auto songs = ScanSongs(&error);
    if (songs.empty()) {
        return ReportError(error);
    }
    const int index = FindSong(songs, song_name, artist, false);
    if (index < 0) {
        return ReportError("未找到匹配的歌曲（歌名：" + song_name + "，歌手：" + artist +
               "）。请确认SD卡 music/mp3 目录中有对应文件。");
    }
    return QueueSong(songs[index]);
}

std::string DogMedia::Step(int direction) {
    std::string error;
    const auto songs = ScanSongs(&error);
    if (songs.empty()) {
        return ReportError(error);
    }
    std::string current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = current_path_;
    }
    int index = direction > 0 ? -1 : 0;
    for (size_t i = 0; i < songs.size(); ++i) {
        if (songs[i].path == current) {
            index = static_cast<int>(i);
            break;
        }
    }
    const int count = static_cast<int>(songs.size());
    index = (index + direction + count) % count;
    return QueueSong(songs[index]);
}

std::string DogMedia::Next() { return Step(1); }
std::string DogMedia::Prev() { return Step(-1); }

std::string DogMedia::Stop() {
    std::unique_lock<std::mutex> command_lock(command_mutex_);
    auto callback = StopLocked();
    command_lock.unlock();
    if (callback) callback(false);
    return "已停止本地音乐播放";
}

std::function<void(bool)> DogMedia::StopLocked() {
    std::function<void(bool)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        pending_ = false;
        playback_requested_ = false;
        if (playing_) callback = playback_callback_;
        playing_ = false;
        if (alarm_inflight_) {
            alarm_inflight_ = false;
            DisableAlarmLocked();
        }
        if (task_) {
            xTaskNotifyGive(task_);
        }
    }
    Application::GetInstance().GetAudioService().CancelLocalPlayback();
    return callback;
}

std::string DogMedia::TogglePlayback() {
    std::unique_lock<std::mutex> command_lock(command_mutex_);
    std::string current;
    bool requested;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requested = playback_requested_;
        current = current_path_;
    }
    if (requested) {
        auto callback = StopLocked();
        command_lock.unlock();
        if (callback) callback(false);
        return "已停止本地音乐播放";
    }

    Song song{FileName(current), current};
    struct stat info = {};
    if (current.empty() || stat(current.c_str(), &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_size <= 0 || !HasMp3Extension(current)) {
        std::string error;
        const auto songs = ScanSongs(&error);
        if (songs.empty()) return ReportError(error);
        song = songs.front();
    }
    std::string error;
    if (!RequestSongLocked(song, 0, error)) return ReportError("播放失败: " + error);
    return "本地歌曲已加入播放队列：" + song.name;
}

std::string DogMedia::ReportError(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = error;
    return error;
}

std::string DogMedia::Recommend() {
    std::string error;
    auto songs = ScanSongs(&error);
    if (songs.empty()) {
        return ReportError(error);
    }
    const size_t count = std::min<size_t>(5, songs.size());
    std::string result = "随机推荐 " + std::to_string(count) + " 首歌曲：";
    for (size_t i = 0; i < count; ++i) {
        const size_t choice = i + esp_random() % (songs.size() - i);
        std::swap(songs[i], songs[choice]);
        result += "\n" + std::to_string(i + 1) + ". " + songs[i].name;
    }
    return result;
}

void DogMedia::LoadAlarm() {
    nvs_handle_t handle = 0;
    if (nvs_open("alarm", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    auto read_int = [&](const char* key, int& value) {
        int32_t stored = value;
        if (nvs_get_i32(handle, key, &stored) == ESP_OK) {
            value = stored;
        }
    };
    read_int("year", alarm_.year);
    read_int("month", alarm_.month);
    read_int("day", alarm_.day);
    read_int("hour", alarm_.hour);
    read_int("minute", alarm_.minute);
    read_int("music_index", alarm_.music_index);
    uint8_t enabled = 0;
    nvs_get_u8(handle, "enabled", &enabled);
    alarm_.enabled = enabled != 0;
    size_t size = 0;
    if (nvs_get_str(handle, "music_path", nullptr, &size) == ESP_OK && size > 0 && size <= 1024) {
        std::vector<char> path(size);
        if (nvs_get_str(handle, "music_path", path.data(), &size) == ESP_OK) {
            alarm_.music_path = path.data();
        }
    }
    nvs_close(handle);
    time_t timestamp;
    if (alarm_.year < 2025 || alarm_.year > 2099 ||
        !ValidDate(alarm_.year, alarm_.month, alarm_.day, alarm_.hour, alarm_.minute, timestamp)) {
        alarm_ = Alarm{};
    }
    ESP_LOGI(kTag, "Alarm settings reloaded: %04d-%02d-%02d %02d:%02d, enabled: %d, music_index: %d, path: %s",
             alarm_.year, alarm_.month, alarm_.day, alarm_.hour, alarm_.minute,
             alarm_.enabled, alarm_.music_index, alarm_.music_path.c_str());
}

bool DogMedia::SaveAlarm(const Alarm& alarm, std::string& error) {
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open("alarm", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        auto set_int = [&](const char* key, int value) {
            if (result == ESP_OK) {
                result = nvs_set_i32(handle, key, value);
            }
        };
        set_int("year", alarm.year);
        set_int("month", alarm.month);
        set_int("day", alarm.day);
        set_int("hour", alarm.hour);
        set_int("minute", alarm.minute);
        set_int("music_index", alarm.music_index);
        if (result == ESP_OK) result = nvs_set_str(handle, "music_path", alarm.music_path.c_str());
        if (result == ESP_OK) result = nvs_set_u8(handle, "enabled", alarm.enabled ? 1 : 0);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result != ESP_OK) {
        error = "闹钟保存失败: " + std::string(esp_err_to_name(result));
        return false;
    }
    return true;
}

std::string DogMedia::SetAlarm(int year, int month, int day, int hour, int minute,
                              const std::string& song_name, const std::string& artist, bool enabled) {
    if (!Start()) {
        return "闹钟设置失败: " + LastError();
    }
    std::tm today = {};
    if ((year == 0 || month == 0 || day == 0) && !ValidClock(today)) {
        return "系统时间尚未同步，无法确定今天的日期。请联网校时后设置闹钟。";
    }
    if (year == 0) year = today.tm_year + 1900;
    if (month == 0) month = today.tm_mon + 1;
    if (day == 0) day = today.tm_mday;
    if (year < 2025 || year > 2099) return "年份无效，应为2025至2099年";
    if (month < 1 || month > 12) return "月份无效，应为1至12月";
    if (day < 1 || day > 31) return "日期无效";
    if (hour < 0 || hour > 23) return "小时无效，应为0至23";
    if (minute < 0 || minute > 59) return "分钟无效，应为0至59";
    time_t alarm_time;
    if (!ValidDate(year, month, day, hour, minute, alarm_time)) {
        return "日期无效，请检查该月份的实际天数";
    }
    if (enabled && ValidClock(today) && time(nullptr) >= alarm_time + 60) {
        return "闹钟时间已经过去，请提供将来的时间";
    }
    Alarm updated;
    updated.year = year;
    updated.month = month;
    updated.day = day;
    updated.hour = hour;
    updated.minute = minute;
    updated.enabled = enabled;
    if (!enabled && song_name.empty() && artist.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        updated.music_path = alarm_.music_path;
        updated.music_index = alarm_.music_index;
    } else {
        if (SearchKey(song_name).empty() && SearchKey(artist).empty()) {
            return "请提供 song_name（歌名）或 artist（歌手），用于匹配本地闹钟音乐";
        }
        std::string scan_error;
        auto songs = ScanSongs(&scan_error);
        if (songs.empty()) return ReportError(scan_error);
        const int index = FindSong(songs, song_name, artist, true);
        if (index < 0) return "未找到匹配的歌曲，闹钟未修改";
        updated.music_index = index;
        updated.music_path = songs[index].path;
    }
    std::string error;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!SaveAlarm(updated, error)) return error;
        if (alarm_inflight_) { ++generation_; pending_ = false; playback_requested_ = false; }
        alarm_ = updated;
        ++alarm_revision_;
        alarm_inflight_ = false;
        alarm_save_pending_ = false;
        xTaskNotifyGive(task_);
    }
    ESP_LOGI(kTag, "SetAlarm saved: %04d-%02d-%02d %02d:%02d index=%d path=%s enabled=%d",
             year, month, day, hour, minute, updated.music_index, updated.music_path.c_str(), enabled);
    return "闹钟已设置为: " + Calendar(year, month, day, hour, minute) +
           "，音乐: " + FileName(updated.music_path) + "，状态: " + (enabled ? "开启" : "关闭");
}

std::string DogMedia::GetAlarm() {
    if (!Start()) return "闹钟读取失败: " + LastError();
    std::lock_guard<std::mutex> lock(mutex_);
    if (alarm_.year == 0) return "当前未设置闹钟";
    return "当前闹钟设置: " + Calendar(alarm_.year, alarm_.month, alarm_.day, alarm_.hour, alarm_.minute) +
           ", 音乐: " + FileName(alarm_.music_path) + ", 状态: " + (alarm_.enabled ? "开启" : "关闭");
}

std::string DogMedia::GetAlarmField(const std::string& field) {
    if (!Start()) return "读取失败";
    Alarm snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = alarm_;
    }
    if (field == "time") return snapshot.year ? Calendar(snapshot.year, snapshot.month, snapshot.day,
                                                        snapshot.hour, snapshot.minute) : "未设置";
    if (field == "music") return snapshot.music_path.empty() ? "未选音乐" : FileName(snapshot.music_path);
    if (field == "state") return !snapshot.year ? "未设置" : snapshot.enabled ? "开启" : "关闭";
    return "";
}

std::string DogMedia::ToggleAlarm() {
    if (!Start()) return "闹钟设置失败: " + LastError();
    std::lock_guard<std::mutex> lock(mutex_);
    if (alarm_.year == 0 || alarm_.music_path.empty()) {
        return "当前未设置闹钟，请先指定时间和本地歌曲";
    }
    Alarm updated = alarm_;
    updated.enabled = !updated.enabled;
    if (updated.enabled) {
        time_t alarm_time;
        std::tm current = {};
        if (!ValidDate(updated.year, updated.month, updated.day, updated.hour, updated.minute, alarm_time) ||
            (ValidClock(current) && time(nullptr) >= alarm_time + 60)) {
            return "闹钟时间已经过去，请重新设置日期和时间";
        }
        struct stat file = {};
        if (stat(updated.music_path.c_str(), &file) != 0 || !S_ISREG(file.st_mode) || file.st_size <= 0) {
            return "闹钟歌曲不存在，请检查SD卡后重新设置";
        }
    }
    std::string error;
    if (!SaveAlarm(updated, error)) return error;
    if (alarm_inflight_) { ++generation_; pending_ = false; playback_requested_ = false; }
    alarm_ = std::move(updated);
    ++alarm_revision_;
    alarm_inflight_ = false;
    alarm_save_pending_ = false;
    xTaskNotifyGive(task_);
    return alarm_.enabled ? "已开启闹钟" : "已关闭闹钟";
}

void DogMedia::ReloadSettings() {
    Settings settings("display");
    const int mode = settings.GetInt("music_mode", 1);
    std::lock_guard<std::mutex> lock(mutex_);
    music_mode_ = mode >= 0 && mode <= 2 ? mode : 1;
}

void DogMedia::SetPlaybackCallback(std::function<void(bool)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    playback_callback_ = std::move(callback);
}

void DogMedia::DisableAlarmLocked() {
    alarm_.enabled = false;
    std::string error;
    alarm_save_pending_ = !SaveAlarm(alarm_, error);
    if (alarm_save_pending_) {
        alarm_save_error_ = "闹钟关闭状态尚未保存，将继续重试: " + error;
        last_error_ = alarm_save_error_;
        ESP_LOGE(kTag, "%s", last_error_.c_str());
    } else alarm_save_error_.clear();
}

void DogMedia::PlaybackStarted(uint32_t generation) {
    std::function<void(bool)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation_.load() != generation) return;
        if (!playing_) callback = playback_callback_;
        playing_ = true;
        if (alarm_inflight_ && alarm_generation_ == generation && alarm_.enabled) {
            // An accepted first PCM frame confirms actual playback. An earlier
            // queue acknowledgement must not consume a still-silent alarm.
            DisableAlarmLocked();
        }
    }
    if (callback) callback(true);
}

void DogMedia::CheckAlarm() {
    std::tm current = {};
    if (!ValidClock(current)) return;
    Alarm due;
    uint32_t revision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (alarm_save_pending_) {
            std::string error;
            if (SaveAlarm(alarm_, error)) {
                alarm_save_pending_ = false;
                if (last_error_ == alarm_save_error_) last_error_.clear();
                alarm_save_error_.clear();
            }
        }
        if (!alarm_.enabled) return;
        time_t alarm_time;
        if (!ValidDate(alarm_.year, alarm_.month, alarm_.day, alarm_.hour, alarm_.minute, alarm_time)) return;
        const time_t now = time(nullptr);
        if (now < alarm_time) return;
        if (now >= alarm_time + 60) {
            // A busy device or missing SD file is a missed alarm, not a played
            // alarm. Keep the failure visible in the music/status surface.
            DisableAlarmLocked();
            last_error_ = "闹钟已超时，设备忙碌或歌曲不可用，未能播放" +
                (alarm_save_pending_ ? "；" + alarm_save_error_ : "");
            return;
        }
        if (alarm_inflight_) return;
        due = alarm_;
        revision = alarm_revision_;
    }
    // A long TTS reply must not exhaust DecodeFile's short interactive wait and
    // permanently consume the alarm. Retry on the normal one-second poll.
    const auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle && state != kDeviceStateListening &&
        state != kDeviceStateWifiConfiguring && state != kDeviceStateActivating) return;
    std::string error;
    const auto songs = ScanSongs(&error);
    int index = -1;
    for (size_t i = 0; i < songs.size(); ++i) {
        if (songs[i].path == due.music_path) index = static_cast<int>(i);
    }
    // Preserve path-first, index-fallback behavior after SD changes.
    if (index < 0 && due.music_index >= 0 && due.music_index < static_cast<int>(songs.size())) {
        ESP_LOGW(kTag, "Play by path failed, fallback to index");
        index = due.music_index;
    }
    if (index < 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = songs.empty() ? error : "闹钟已到时间，但SD卡中没有找到设置的歌曲";
        ESP_LOGE(kTag, "%s", last_error_.c_str());
        return;
    }
    ESP_LOGI(kTag, "Playing alarm music by path: %s", songs[index].path.c_str());
    if (!RequestSong(songs[index], 3, error, revision) && !error.empty()) ReportError(error);
}

std::string DogMedia::CurrentSong() {
    std::lock_guard<std::mutex> lock(mutex_);
    return FileName(current_path_);
}

std::string DogMedia::LastError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

bool DogMedia::IsPlaying() {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_;
}

bool DogMedia::HasPlaybackRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    return playback_requested_;
}

void DogMedia::Worker() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        CheckAlarm();
        std::string path;
        uint32_t generation;
        int policy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending_) continue;
            path = pending_path_;
            pending_ = false;
            generation = generation_.load();
            policy = pending_policy_;
        }
        std::string error;
        bool completed = false;
        const bool played = DecodeFile(path, generation, completed, error);
        const bool advance = played && completed && policy != 2 && policy != 3;
        std::function<void(bool)> playback_callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (alarm_inflight_ && alarm_generation_ == generation) alarm_inflight_ = false;
            if (generation_.load() == generation && !advance) playback_requested_ = false;
            if (playing_) playback_callback = playback_callback_;
            playing_ = false;
            if (generation_.load() == generation && !error.empty()) {
                last_error_ = error;
                ESP_LOGE(kTag, "Failed to play file in async task: %s", error.c_str());
            }
        }
        if (playback_callback) playback_callback(false);
        if (advance && generation_.load() == generation) {
            std::string scan_error;
            const auto songs = policy == 1 ? std::vector<Song>{{FileName(path), path}} : ScanSongs(&scan_error);
            if (!songs.empty()) {
                size_t next = 0;
                for (size_t i = 0; i < songs.size(); ++i) {
                    if (songs[i].path == path) next = (i + 1) % songs.size();
                }
                std::lock_guard<std::mutex> lock(mutex_);
                if (generation_.load() == generation) {
                    // Settings: single=0, random=1, sequence=2.
                    if (music_mode_ == 0) {
                        for (size_t i = 0; i < songs.size(); ++i) {
                            if (songs[i].path == path) next = i;
                        }
                    } else if (music_mode_ == 1) {
                        next = esp_random() % songs.size();
                    }
                    pending_path_ = songs[next].path;
                    pending_policy_ = policy;
                    current_path_ = pending_path_;
                    pending_ = true;
                    playback_requested_ = true;
                    ++generation_;
                    xTaskNotifyGive(task_);
                    ESP_LOGI(kTag, "Auto-advance next track: %s", pending_path_.c_str());
                }
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                if (generation_.load() == generation) {
                    playback_requested_ = false;
                    last_error_ = scan_error;
                }
            }
        }
    }
}

bool DogMedia::DecodeFile(const std::string& path, uint32_t generation, bool& completed,
                          std::string& error) {
    auto& app = Application::GetInstance();
    // MCP can arrive during TTS. Waiting happens on this worker, allowing the
    // application event loop to receive the speaking-stop event.
    const int64_t deadline = esp_timer_get_time() + 4000000;
    while (app.GetDeviceState() == kDeviceStateSpeaking && esp_timer_get_time() < deadline) {
        if (generation_.load() != generation) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (app.GetDeviceState() == kDeviceStateListening) {
        // StopListening schedules both the protocol notification and the state
        // transition on the application task. Changing only the audio-service
        // bits would leave the application falsely reporting Listening.
        app.StopListening();
        const int64_t idle_deadline = esp_timer_get_time() + 1500000;
        while (app.GetDeviceState() == kDeviceStateListening && esp_timer_get_time() < idle_deadline) {
            if (generation_.load() != generation) return false;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    const auto state = app.GetDeviceState();
    if (state != kDeviceStateIdle && state != kDeviceStateWifiConfiguring && state != kDeviceStateActivating) {
        error = "当前设备正在对话或升级，无法播放本地音乐";
        return false;
    }
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb"), &fclose);
    if (!file) {
        error = "无法打开歌曲: " + FileName(path);
        return false;
    }
    esp_audio_simple_dec_cfg_t config = {};
    config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    config.use_frame_dec = false;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    auto result = esp_audio_simple_dec_open(&config, &decoder);
    if (result != ESP_AUDIO_ERR_OK) {
        error = "MP3解码器打开失败: " + std::to_string(result);
        return false;
    }
    struct DecoderCloser {
        esp_audio_simple_dec_handle_t value;
        ~DecoderCloser() { esp_audio_simple_dec_close(value); }
    } closer{decoder};
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr || codec->output_channels() != 1) {
        error = "音频输出不可用或不是单声道";
        return false;
    }
    auto& audio = app.GetAudioService();
    // A speaking-stop event can precede its final queued audio frame. Wait on
    // the actual service queue/in-flight state before attempting ownership.
    const int64_t drain_deadline = esp_timer_get_time() + 1500000;
    while (!audio.IsIdle() && esp_timer_get_time() < drain_deadline &&
           generation_.load() == generation && app.GetDeviceState() == state) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    struct AcquireRequest {
        std::mutex mutex;
        bool done = false;
        bool cancelled = false;
        bool acquired = false;
    };
    // Acquire on the application task, which also handles scheduled TTS start /
    // stop. Its local-owner guard and this transition are therefore serialized:
    // a tool-result TTS cannot arrive between the Idle check and ownership.
    auto request = std::make_shared<AcquireRequest>();
    app.Schedule([this, request, generation]() {
        std::lock_guard<std::mutex> lock(request->mutex);
        auto& application = Application::GetInstance();
        const auto current = application.GetDeviceState();
        if (!request->cancelled && generation_.load() == generation &&
            (current == kDeviceStateIdle || current == kDeviceStateWifiConfiguring ||
             current == kDeviceStateActivating)) {
            request->acquired = application.GetAudioService().BeginLocalPlayback();
        }
        request->done = true;
    });
    bool acquired = false;
    const int64_t audio_deadline = esp_timer_get_time() + 2500000;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            if (request->done) {
                acquired = request->acquired;
                break;
            }
            if (generation_.load() != generation || esp_timer_get_time() >= audio_deadline) {
                request->cancelled = true;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (generation_.load() != generation) {
        if (acquired) audio.EndLocalPlayback();
        return false;
    }
    if (!acquired) {
        error = "音频服务正忙，无法取得本地播放通道";
        return false;
    }
    struct LocalPlaybackCloser {
        AudioService& audio;
        ~LocalPlaybackCloser() { audio.EndLocalPlayback(); }
    } playback{audio};
    ESP_LOGI(kTag, "Suspending audio service for MP3 playback");
    std::vector<uint8_t> input(4096);
    std::vector<int16_t> pcm(2304);
    size_t buffered = 0;
    bool eof = false;
    bool had_output = false;
    MusicResampler resampler;
    int64_t next_alarm_check = esp_timer_get_time() + 1000000;
    while (generation_.load() == generation) {
        if (esp_timer_get_time() >= next_alarm_check) {
            CheckAlarm();
            next_alarm_check = esp_timer_get_time() + 1000000;
            if (generation_.load() != generation) break;
        }
        if (!eof && buffered < input.size()) {
            const size_t read = fread(input.data() + buffered, 1, input.size() - buffered, file.get());
            buffered += read;
            eof = feof(file.get());
            if (ferror(file.get())) {
                error = "读取歌曲失败: " + FileName(path);
                break;
            }
        }
        if (buffered == 0 && eof) {
            completed = had_output;
            break;
        }
        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = input.data();
        raw.len = buffered;
        raw.eos = eof;
        esp_audio_simple_dec_out_t out = {};
        out.buffer = reinterpret_cast<uint8_t*>(pcm.data());
        out.len = pcm.size() * sizeof(int16_t);
        result = esp_audio_simple_dec_process(decoder, &raw, &out);
        if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (out.needed_size <= out.len || out.needed_size > 64 * 1024) {
                error = "MP3解码器请求了无效的PCM缓冲区";
                break;
            }
            pcm.resize((out.needed_size + sizeof(int16_t) - 1) / sizeof(int16_t));
            continue;
        }
        if (result != ESP_AUDIO_ERR_OK) {
            error = "MP3解码失败: " + std::to_string(result);
            break;
        }
        if (out.decoded_size > 0) {
            esp_audio_simple_dec_info_t info = {};
            result = esp_audio_simple_dec_get_info(decoder, &info);
            if (result != ESP_AUDIO_ERR_OK || info.bits_per_sample != 16 ||
                (info.channel != 1 && info.channel != 2) || info.sample_rate < 8000 || info.sample_rate > 48000 ||
                out.decoded_size > pcm.size() * sizeof(int16_t) ||
                out.decoded_size % (sizeof(int16_t) * info.channel) != 0) {
                error = "歌曲音频格式不受支持";
                break;
            }
            auto samples = resampler.Process(pcm.data(), out.decoded_size / sizeof(int16_t) / info.channel,
                                               info.channel, info.sample_rate, codec->output_sample_rate());
            if (!samples.empty()) {
                if (generation_.load() != generation) break;
                if (!audio.WriteLocalPlayback(std::move(samples))) {
                    error = "本地音乐被音频服务中断";
                    break;
                }
                PlaybackStarted(generation);
                had_output = true;
            }
        }
        if (raw.consumed > buffered) {
            error = "MP3解码器返回了无效的输入长度";
            break;
        }
        buffered -= raw.consumed;
        if (buffered > 0 && raw.consumed > 0) {
            memmove(input.data(), input.data() + raw.consumed, buffered);
        }
        if (raw.consumed == 0 && out.decoded_size == 0) {
            if (eof) {
                completed = had_output;
                break;
            }
            if (buffered == input.size()) {
                if (input.size() >= 64 * 1024) {
                    error = "MP3数据无进展或文件头过大";
                    break;
                }
                input.resize(input.size() * 2);
            }
        }
    }
    if (!had_output && error.empty() && generation_.load() == generation) {
        error = "歌曲文件没有可解码的MP3音频";
    }
    ESP_LOGI(kTag, "Resuming audio service after MP3 playback");
    return had_output;
}
