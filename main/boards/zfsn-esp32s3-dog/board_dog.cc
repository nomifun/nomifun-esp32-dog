// ZFSN ESP32-S3 desktop robot dog board.

#include <sdkconfig.h>
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "settings.h"
#include "mcp_server.h"
#include "assets/lang_config.h"
#include "system_info.h"

#include "display_board.h"
#include "pet_dog.h"
#include "power_manager.h"
#include "ws2812_LED.h"
#include "dog_media.h"
#include "web_control_server.h"
#include "panel_capture.h"
#include "network_reset.h"

#include <wifi_station.h>
#include <ssid_manager.h>
#include <esp_lcd_panel_sh1106.h>

#include <esp_log.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <driver/i2c_master.h>
#include <driver/sdmmc_host.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <cstring>
#include <array>
#include <algorithm>
#include <stdexcept>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <atomic>
#include <cstdio>

static constexpr std::array<const char*, 21> kDogEmotions = {
    "neutral", "happy", "laughing", "funny", "sad", "angry", "crying",
    "loving", "embarrassed", "surprised", "shocked", "thinking", "winking",
    "cool", "relaxed", "delicious", "kissy", "confident", "sleepy", "silly", "confused"
};

static bool IsDogEmotion(const std::string& emotion) {
    return std::any_of(kDogEmotions.begin(), kDogEmotions.end(), [&emotion](const char* name) { return emotion == name; });
}

#define TAG "BoardDog"

// ---------------------------------------------------------------------------
// ButtonManager
// ---------------------------------------------------------------------------
class ButtonManager {
public:
    Button boot_button;
    Button button1;
    Button button2;
    Button button3;

    ButtonManager()
        : boot_button(BOOT_BUTTON_GPIO, false, 5000),
          button1(BUTTON_1_GPIO),
          button2(BUTTON_2_GPIO, false, 650),
          button3(BUTTON_3_GPIO) {
        ESP_LOGI(TAG, "ButtonManager initialized with 4 buttons");
        ESP_LOGI(TAG, "Boot button GPIO: %d", BOOT_BUTTON_GPIO);
        ESP_LOGI(TAG, "Button 1 GPIO: %d", BUTTON_1_GPIO);
        ESP_LOGI(TAG, "Button 2 GPIO: %d", BUTTON_2_GPIO);
        ESP_LOGI(TAG, "Button 3 GPIO: %d", BUTTON_3_GPIO);
    }
};

// ---------------------------------------------------------------------------
// DogSdCard
// ---------------------------------------------------------------------------
class DogSdCard {
public:
    bool Mount() {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.clk = SDCARD_SDMMC_CLK_PIN;
        slot_config.cmd = SDCARD_SDMMC_CMD_PIN;
        slot_config.d0 = SDCARD_SDMMC_D0_PIN;
        slot_config.width = SDCARD_SDMMC_BUS_WIDTH;

        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 0,
            .disk_status_check_enable = true,
        };
        esp_err_t ret = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &card_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
            return false;
        }
        sdmmc_card_print_info(stdout, card_);
        mounted_ = true;
        return true;
    }

    bool IsMounted() const { return mounted_; }

private:
    sdmmc_card_t* card_ = nullptr;
    bool mounted_ = false;
};

// ---------------------------------------------------------------------------
// DogController
// All nine dog tools use ordinary AddTool; AddUserOnlyTool would hide them
// from the cloud's default tools/list request.
// ---------------------------------------------------------------------------
class DogController {
public:
    DogController(PetDog* pet_dog, WS2812LED* led)
        : pet_dog_(pet_dog), led_(led) {}

    void RegisterMcpTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.dog.action",
            "控制狗的动作。必须调用工具后再回复；成功表示动作已接受，实际动作异步执行。action: 动作类型(walk_forward-前进, walk_back-后退, turn_left-向左转, turn_right-向右转, sitdown-坐下/蹲下, stand-站立, sleep-趴下/睡觉, stretch-伸懒腰, scratching-挠痒痒, swing-摇摆/跳舞, petwave-挥挥手, stop-停止, jump-跳跃)。steps: 行走步数(1-100，仅用于walk_forward和walk_back，默认3)。angle: 转动角度(1-360，仅用于turn_left和turn_right，默认30)。",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("steps", kPropertyTypeInteger, 3, 1, 100),
                Property("angle", kPropertyTypeInteger, 30, 1, 360),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto action = properties["action"].value<std::string>();
                int steps = properties["steps"].value<int>();
                int angle = properties["angle"].value<int>();
                if (!pet_dog_->DoAction(action.c_str(), steps, angle)) {
                    ESP_LOGW(TAG, "Dog action rejected: %s steps=%d angle=%d", action.c_str(), steps, angle);
                    throw std::runtime_error("Action unavailable: " + action);
                }
                return true;
            });

        mcp_server.AddTool("self.dog.setting",
            "查询或修改机器狗设置项。当用户要求开启/关闭充电灯效、开机动画、自由探索，或查询当前设置时使用。setting: 设置项(charging_led-充电灯效, boot_animation-开机动画, free_explore-自由探索, all-全部，仅用于查询)。action: 操作(get-查询, set-设置, toggle-切换开关，默认get)。enabled: 是否开启(true/false)，action为set时必填。",
            PropertyList({
                Property("setting", kPropertyTypeString),
                Property("action", kPropertyTypeString, std::string("get")),
                Property("enabled", kPropertyTypeBoolean, false),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto setting = properties["setting"].value<std::string>();
                auto action = properties["action"].value<std::string>();
                Settings settings("display", true);
                auto read_all = [&settings]() -> std::string {
                    char buf[160];
                    snprintf(buf, sizeof(buf),
                        "{\"charging_led\":%s,\"boot_animation\":%s,\"free_explore\":%s}",
                        settings.GetBool("charging_led", true) ? "true" : "false",
                        settings.GetBool("boot_animation", true) ? "true" : "false",
                        settings.GetBool("free_explore", false) ? "true" : "false");
                    return std::string(buf);
                };
                if (action == "get") {
                    if (setting != "all" && setting != "charging_led" && setting != "boot_animation" && setting != "free_explore") {
                        throw std::runtime_error("Unknown setting: " + setting);
                    }
                    return read_all();
                }
                bool enabled = properties["enabled"].value<bool>();
                if (action == "set" && !properties["enabled"].provided()) {
                    throw std::runtime_error("enabled is required when action is set");
                }
                if (setting == "all") {
                    throw std::runtime_error("setting 'all' is query-only");
                }
                if (setting != "charging_led" && setting != "boot_animation" && setting != "free_explore") {
                    throw std::runtime_error("Unknown setting: " + setting);
                }
                if (action == "toggle") {
                    enabled = !settings.GetBool(setting.c_str(), setting != "free_explore");
                } else if (action != "set") {
                    throw std::runtime_error("Unknown setting action: " + action);
                }
                settings.SetBool(setting.c_str(), enabled);
                if (setting == "free_explore") {
                    pet_dog_->SetFreeExplore(enabled);
                } else if (setting == "charging_led") {
                    led_->SetChargingEffectEnabled(enabled);
                }
                return read_all();
            });

        mcp_server.AddTool("self.ws2812_led.control",
            "控制LED灯光效果。mode: 灯光模式(turn_on_off-开关灯, flash_light-闪光灯, water_light-流水灯, breathe_light-呼吸灯, rainbow_light-彩虹灯)。color: 灯光颜色(红灯，橙灯，黄灯，绿灯，蓝灯，紫灯，白灯，关灯)，仅用于turn_on_off模式，默认关灯。brightness: 灯光亮度(0-100)，仅用于turn_on_off模式，默认50。",
            PropertyList({
                Property("mode", kPropertyTypeString),
                Property("color", kPropertyTypeString, std::string("关灯")),
                Property("brightness", kPropertyTypeInteger, 50, 0, 100),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto mode = properties["mode"].value<std::string>();
                auto color = properties["color"].value<std::string>();
                int brightness = properties["brightness"].value<int>();
                if (mode != "turn_on_off" && mode != "flash_light" && mode != "water_light" && mode != "breathe_light" && mode != "rainbow_light") {
                    throw std::runtime_error("Unknown LED mode: " + mode);
                }
                led_->Control(mode, color, brightness);
                return true;
            });

        mcp_server.AddTool("self.display.set_emotion",
            "设置表情、切换表情。emotion: 表情类型(neutral-中性, happy-开心, laughing-大笑, funny-搞笑, sad-悲伤, angry-愤怒, crying-哭泣, loving-爱心, embarrassed-尴尬, surprised-惊讶, shocked-震惊, thinking-思考, winking-眨眼, cool-酷, relaxed-放松, delicious-美味, kissy-亲吻, confident-自信, sleepy-困倦, silly-傻笑, confused-困惑)。",
            PropertyList({
                Property("emotion", kPropertyTypeString),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto emotion = properties["emotion"].value<std::string>();
                if (!IsDogEmotion(emotion)) throw std::runtime_error("Unknown emotion: " + emotion);
                auto display = Board::GetInstance().GetDisplay();
                if (display == nullptr) {
                    throw std::runtime_error("Display unavailable");
                }
                display->SetEmotion(emotion.c_str());
                return true;
            });

        mcp_server.AddTool("self.display.firework",
            "放烟花、播放烟花动画。当用户要求放烟花、庆祝、或者想要看烟花效果时调用此工具。",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto display = dynamic_cast<Sh1106Display*>(Board::GetInstance().GetDisplay());
                if (!display) throw std::runtime_error("Display unavailable");
                display->PlayFirework();
                return std::string("正在播放烟花动画！");
            });

        mcp_server.AddTool("self.music.play",
            "【优先使用】用户要求播放音乐/歌曲时，必须调用本工具从本机SD卡搜索并播放。禁止改用云端 search_music / play_music（会搜错歌、唱错歌）。示例：用户说「播放周杰伦的七里香」→ song_name=七里香, artist=周杰伦。song_name: 歌名关键词（必填，支持模糊匹配）；artist: 歌手关键词（强烈建议填写，支持模糊匹配）。",
            PropertyList({
                Property("song_name", kPropertyTypeString),
                Property("artist", kPropertyTypeString, std::string("")),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                return DogMedia::GetInstance().Play(properties["song_name"].value<std::string>(), properties["artist"].value<std::string>());
            });

        mcp_server.AddTool("self.recommended_music.get_recommended_music",
            "当用户有让你推荐歌曲，问你可以播放哪些歌曲的时候，搜索SD卡里面的歌曲，随机推荐5首歌曲。返回歌曲列表，包含文件名信息。",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                return DogMedia::GetInstance().Recommend();
            });

        mcp_server.AddTool("self.alarm.set_alarm",
            "设置闹钟，并在闹钟时间播放用户指定的本地音乐。当用户要求设置闹钟、定闹钟、设置提醒、或说「X分钟后播放某首歌」时使用此功能。重要：在调用此工具之前，必须先向用户回复确认信息，说明具体闹钟时间以及将播放的歌曲。如果用户说'X分钟后/X小时后'，请根据当前时间计算出具体的年月日时分再调用。如果用户没有提供具体的年月日，则使用今天的日期作为基础。【音乐匹配强制要求】只要用户提到了歌名或歌手，必须传入 song_name 和/或 artist，系统会在SD卡中模糊搜索对应歌曲；禁止省略歌名参数，否则会播放错误的歌曲。year: 年份(2025-2099，可选); month: 月份(1-12，可选); day: 日期(1-31，可选); hour: 小时(0-23); minute: 分钟(0-59); song_name: 歌曲名称关键词（必填，支持模糊匹配，如「七里香」）；artist: 歌手名称关键词（强烈建议填写，支持模糊匹配，如「周杰伦」）；enabled: 是否启用(可选，默认为true)。返回设置结果（含匹配到的实际文件名）。",
            PropertyList({
                Property("year", kPropertyTypeInteger, 0),
                Property("month", kPropertyTypeInteger, 0),
                Property("day", kPropertyTypeInteger, 0),
                Property("hour", kPropertyTypeInteger, 0, 23),
                Property("minute", kPropertyTypeInteger, 0, 59),
                Property("song_name", kPropertyTypeString),
                Property("artist", kPropertyTypeString, std::string("")),
                Property("enabled", kPropertyTypeBoolean, true),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                return DogMedia::GetInstance().SetAlarm(
                    properties["year"].value<int>(), properties["month"].value<int>(), properties["day"].value<int>(),
                    properties["hour"].value<int>(), properties["minute"].value<int>(),
                    properties["song_name"].value<std::string>(), properties["artist"].value<std::string>(),
                    properties["enabled"].value<bool>());
            });

        mcp_server.AddTool("self.alarm.get_alarm",
            "查询当前设置的闹钟信息。返回闹钟的年月日时分、音乐名称和启用状态。",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                return DogMedia::GetInstance().GetAlarm();
            });
    }

private:
    PetDog* pet_dog_;
    WS2812LED* led_;
};

// ---------------------------------------------------------------------------
// BoardDog
// ---------------------------------------------------------------------------
using DogNetworkBoard = WifiBoard;

class BoardDog : public DogNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    Sh1106Display* display_ = nullptr;
    WS2812LED* led_ = nullptr;
    PetDog pet_dog_;
    DogController* dog_controller_ = nullptr;
    DogSdCard sd_card_;
    PowerManager* power_manager_ = nullptr;
    ButtonManager* buttons_ = nullptr;
    WebControlServer* web_server_ = nullptr;

    esp_timer_handle_t volume_inc_timer_ = nullptr;
    esp_timer_handle_t volume_dec_timer_ = nullptr;
    esp_timer_handle_t music_switch_timer_ = nullptr;
    std::atomic<bool> volume_inc_held_{false};
    std::atomic<bool> volume_dec_held_{false};

    std::atomic<bool> network_reset_requested_{false};

    void RequestNetworkReset() {
        if (network_reset_requested_.exchange(true)) return;
        // Recovery must remain available even while the main event loop is
        // waiting on an unavailable server. Never block the button timer task.
        if (xTaskCreate([](void* arg) {
                auto self = static_cast<BoardDog*>(arg);
                self->pet_dog_.SetFreeExplore(false);
                self->pet_dog_.stop();
                DogMedia::GetInstance().Stop();
                // GPIO0 is also a boot strap. Let the user release BOOT before
                // restarting; the settings-menu path normally skips this wait.
                if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    self->GetDisplay()->ShowNotification("请松开BOOT键");
                    const auto started = xTaskGetTickCount();
                    while (gpio_get_level(BOOT_BUTTON_GPIO) == 0 &&
                           xTaskGetTickCount() - started < pdMS_TO_TICKS(20000)) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                        self->GetDisplay()->ShowNotification("松开BOOT后重试");
                        self->network_reset_requested_.store(false);
                        vTaskDelete(nullptr);
                        return;
                    }
                }
                self->GetDisplay()->ShowNotification("正在重置网络设置");
                const auto error = ResetDogNetworkSettings();
                if (error != ESP_OK) {
                    ESP_LOGE(TAG, "Network reset failed: %s", esp_err_to_name(error));
                    self->GetDisplay()->ShowNotification("重置失败，请重试");
                    self->network_reset_requested_.store(false);
                    vTaskDelete(nullptr);
                    return;
                }
                ESP_LOGI(TAG, "Connection settings reset; restarting into WiFi provisioning");
                self->GetDisplay()->ShowNotification("网络已重置，正在重启");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }, "ResetNetwork", 4096, this, 4, nullptr) != pdPASS) {
            network_reset_requested_.store(false);
            ESP_LOGE(TAG, "Failed to create network reset task");
            GetDisplay()->ShowNotification("重置启动失败，请重试");
        }
    }

    void ChangeVolume(int delta) {
        Application::GetInstance().Schedule([this, delta]() {
            // A queued repeat may run after key release or after leaving this
            // page. Do not keep changing volume in either case.
            if (!display_ || !display_->VolumePageActive() ||
                !(delta > 0 ? volume_inc_held_.load() : volume_dec_held_.load())) {
                esp_timer_stop(delta > 0 ? volume_inc_timer_ : volume_dec_timer_);
                return;
            }
            auto codec = GetAudioCodec();
            if (codec) codec->SetOutputVolume(std::clamp(codec->output_volume() + delta, 0, 100));
        });
    }

    void InitializeI2c() {
        ESP_LOGI(TAG, "Init I2C for display: port=%d sda=%d scl=%d",
                 (int)I2C_NUM_0, (int)DISPLAY_SDA_PIN, (int)DISPLAY_SCL_PIN);
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        esp_err_t ret = i2c_new_master_bus(&bus_config, &i2c_bus_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
            return;
        }
        ESP_LOGI(TAG, "I2C bus created for display");
    }

    void InitializeSH1106Display() {
        ESP_LOGI(TAG, "Create panel IO (I2C) dev_addr=0x%02X, width=%d, height=%d, mirrorX=%d, mirrorY=%d",
                 0x3C, DISPLAY_WIDTH, DISPLAY_HEIGHT, (int)DISPLAY_MIRROR_X, (int)DISPLAY_MIRROR_Y);
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        if (esp_lcd_new_panel_io_i2c_v2(i2c_bus_, &io_config, &panel_io_) != ESP_OK) {
            ESP_LOGE(TAG, "esp_lcd_new_panel_io_i2c_v2 failed");
            return;
        }

        ESP_LOGI(TAG, "Install SH1106 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = nullptr;
            return;
        }
        // This bundled SH1106 driver's invert_color() is a no-op. Its own
        // command table defines A6 as normal RAM mode; set it explicitly so a
        // soft reboot cannot inherit a previous application's reverse mode.
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, 0xA6, nullptr, 0));
        ESP_LOGI(TAG, "SH1106 normal RAM mode set (A6; page bit 1 lights pixel)");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

#ifdef CONFIG_ZFSN_UI_CAPTURE
        ESP_ERROR_CHECK(InstallDogPanelCapture(panel_));
#endif

        display_ = new Sh1106Display(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

#ifdef CONFIG_ZFSN_UI_CAPTURE
    void StartUiCapturePreview() {
        // This explicit acceptance build previews pages only. It never invokes
        // menu confirmation, changes a setting, plays audio or moves the dog.
        if (xTaskCreate([](void* arg) {
                auto self = static_cast<BoardDog*>(arg);
                vTaskDelay(pdMS_TO_TICKS(1800));
                constexpr const char* names[] = {"apps", "settings", "music", "alarm", "about", "volume", "reset", "emotion"};
                int captured = 0;
                ESP_LOGI(TAG, "OLED_CAPTURE_PREVIEW_BEGIN");
                for (int page = 0; page < int(std::size(names)); ++page) {
                    const int count = self->display_->UiCaptureSceneItemCount(page);
                    for (int selection = 0; selection < count; ++selection) {
                        char label[48];
                        snprintf(label, sizeof(label), "%s_%d", names[page], selection);
                        if (!self->display_->ShowUiCaptureScene(page, selection, label)) {
                            ESP_LOGE(TAG, "OLED capture scene request failed");
                            goto finished;
                        }
                        esp_err_t result = ESP_ERR_NOT_FINISHED;
                        for (int retry = 0; retry < 8 && result == ESP_ERR_NOT_FINISHED; ++retry) {
                            vTaskDelay(pdMS_TO_TICKS(150));
                            result = DumpDogPanelFrame();
                        }
                        if (result != ESP_OK) {
                            ESP_LOGE(TAG, "OLED capture frame unavailable: %s", esp_err_to_name(result));
                            goto finished;
                        }
                        ++captured;
                    }
                }
finished:
                self->display_->ExitMenu();
                ESP_LOGI(TAG, "OLED_CAPTURE_PREVIEW_END frames=%d", captured);
                vTaskDelete(nullptr);
            }, "OledCapture", 6144, this, 1, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "OLED capture task creation failed");
        }
    }
#endif

    void InitializeVolumeTimers() {
        esp_timer_create_args_t inc_args = {
            .callback = [](void* arg) {
                static_cast<BoardDog*>(arg)->ChangeVolume(2);
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vol_inc",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&inc_args, &volume_inc_timer_));

        esp_timer_create_args_t dec_args = {
            .callback = [](void* arg) {
                static_cast<BoardDog*>(arg)->ChangeVolume(-2);
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vol_dec",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&dec_args, &volume_dec_timer_));

        // music_switch — next-track key timer.
        esp_timer_create_args_t music_switch_args = {
            .callback = &BoardDog::MusicSwitchTimerCb,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "music_switch",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&music_switch_args, &music_switch_timer_));
    }

    static void MusicSwitchTimerCb(void* arg) {
        auto self = static_cast<BoardDog*>(arg);
        Application::GetInstance().Schedule([self]() {
            if ((!self->display_ || !self->display_->MenuActive()) && DogMedia::GetInstance().IsPlaying())
                DogMedia::GetInstance().Next();
        });
    }

    void InitializeButtons() {
        buttons_ = new ButtonManager();

        // Button mappings. Holding key 1 while double-clicking key 3 runs the
        // servo test.
        buttons_->boot_button.OnClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (display_) display_->ExitMenu();
                auto& app = Application::GetInstance();
                if (DogMedia::GetInstance().HasPlaybackRequest()) {
                    // Only a full stop is available; resuming a paused MP3
                    // position is not supported by the player.
                    DogMedia::GetInstance().Stop();
                } else {
                    app.ToggleChatState();
                }
            });
        });
        buttons_->boot_button.OnLongPress([this]() {
            RequestNetworkReset();
        });
        buttons_->button1.OnClick([this]() {
            Application::GetInstance().Schedule([this]() { if (display_) display_->MenuNext(); });
        });
        buttons_->button3.OnClick([this]() {
            Application::GetInstance().Schedule([this]() { if (display_) display_->MenuPrevious(); });
        });
        buttons_->button1.OnPressDown([this]() { volume_inc_held_.store(true); });
        buttons_->button3.OnPressDown([this]() { volume_dec_held_.store(true); });
        buttons_->button1.OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() {
                if (volume_inc_held_.load() && display_ && display_->VolumePageActive()) {
                    ChangeVolume(2);
                    esp_timer_start_periodic(volume_inc_timer_, 150000);
                }
            });
        });
        buttons_->button3.OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() {
                if (volume_dec_held_.load() && display_ && display_->VolumePageActive()) {
                    ChangeVolume(-2);
                    esp_timer_start_periodic(volume_dec_timer_, 150000);
                }
            });
        });
        buttons_->button1.OnPressUp([this]() {
            volume_inc_held_.store(false);
            esp_timer_stop(volume_inc_timer_);
        });
        buttons_->button3.OnPressUp([this]() {
            volume_dec_held_.store(false);
            esp_timer_stop(volume_dec_timer_);
        });
        buttons_->button2.OnClick([this]() {
            Application::GetInstance().Schedule([this]() { if (display_) display_->MenuConfirm(); });
        });
        buttons_->button2.OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() { if (display_) display_->MenuBack(); });
        });
        buttons_->button2.OnDoubleClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if ((!display_ || !display_->MenuActive()) && DogMedia::GetInstance().IsPlaying())
                    DogMedia::GetInstance().Next();
            });
        });
        buttons_->button3.OnDoubleClick([this]() {
            const bool servo_test = gpio_get_level(BUTTON_1_GPIO) == 0;
            Application::GetInstance().Schedule([this, servo_test]() {
                if (servo_test) {
                    ESP_LOGI(TAG, "Servo test: swing dance (button1 held + button3 double-click)");
                    pet_dog_.DoAction("swing", DEFAULT_STEPS, DEFAULT_TURN_ANGLE);
                } else if ((!display_ || !display_->MenuActive()) && DogMedia::GetInstance().IsPlaying()) {
                    esp_timer_start_once(music_switch_timer_, 200000);
                }
            });
        });
    }

    void InitializeSdCard() {
        sd_card_.Mount();
    }

    void InitializePetDog() {
        // Argument order: (IO_1=14, IO_2=17, IO_3=13, IO_4=18)
        pet_dog_.InitializeDog(SERVO_PWM2_GPIO, SERVO_PWM1_GPIO, SERVO_PWM3_GPIO, SERVO_PWM4_GPIO);
    }

    void InitializeWiFi() {
        WebControlServer::Callbacks callbacks;
        callbacks.sd_mounted = [this]() { return sd_card_.IsMounted(); };
        callbacks.set_emotion = [this](const std::string& emotion) {
            if (!display_ || !IsDogEmotion(emotion)) return false;
            display_->SetEmotion(emotion.c_str());
            return true;
        };
        callbacks.random_emotion = [this]() {
            if (!display_) return false;
            display_->SetEmotion(kDogEmotions[esp_random() % kDogEmotions.size()]);
            return true;
        };
        callbacks.load_custom_emotion = [this](const std::string& name) {
            if (!display_ || !sd_card_.IsMounted() || name.empty() || name.find('/') != std::string::npos ||
                name.find('\\') != std::string::npos || name.find("..") != std::string::npos) return false;
            return display_->LoadCustomEmotion(name);
        };
        callbacks.get_volume = [this]() { return GetAudioCodec()->output_volume(); };
        callbacks.play_music = [](const std::string& path, bool loop) {
            std::string error;
            bool accepted = DogMedia::GetInstance().QueueFile(path, loop, error);
            if (!accepted) ESP_LOGW(TAG, "Custom emotion music rejected: %s", error.c_str());
            return accepted;
        };
        callbacks.set_volume = [this](int volume) { GetAudioCodec()->SetOutputVolume(volume); return true; };
        callbacks.settings_changed = [this]() {
            DogMedia::GetInstance().ReloadSettings();
            Settings settings("display");
            led_->SetChargingEffectEnabled(settings.GetBool("charging_led", true));
            return true;
        };
        callbacks.wake = [this]() {
            DogMedia::GetInstance().Stop();
            Application::GetInstance().Schedule([this]() { if (display_) display_->ExitMenu(); });
            Application::GetInstance().SetChatActive(true);
            return true;
        };
        callbacks.sleep = []() {
            DogMedia::GetInstance().Stop();
            Application::GetInstance().SetChatActive(false);
            return true;
        };
        web_server_ = new WebControlServer(pet_dog_, *led_, std::move(callbacks));
        ESP_LOGI(TAG, "About to register WiFi event handler");
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &BoardDog::WifiEventHandler, this));
    }

    static void WifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
        if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) {
            return;
        }
        auto self = static_cast<BoardDog*>(arg);
        ESP_LOGI(TAG, "WiFi connected event received");
        Application::GetInstance().Schedule([self]() {
            // The provisioning AP already owns port 80 while testing entered
            // credentials. Start normal control routes only after its reboot.
            if (Application::GetInstance().GetDeviceState() == kDeviceStateWifiConfiguring) return;
            ESP_LOGI(TAG, "WiFi connected, starting web control server");
            if (self->web_server_) {
                esp_err_t result = self->web_server_->Start();
                if (result != ESP_OK) ESP_LOGE(TAG, "Web control start failed: %s", esp_err_to_name(result));
            }
        });
    }

public:
    BoardDog() {
        InitializeI2c();
        InitializeSH1106Display();
        InitializeSdCard();
        DogMedia::GetInstance().Start();
        InitializePetDog();

        Settings settings("display", true);
        pet_dog_.SetFreeExplore(settings.GetBool("free_explore", false));

        power_manager_ = new PowerManager(POWER_CHARGE_DETECT_PIN, POWER_ADC_UNIT, POWER_ADC_CHANNEL);
        led_ = new WS2812LED(WS2812_LED_GPIO, WS2812_LED_COUNT);
        led_->SetChargingEffectEnabled(settings.GetBool("charging_led", true));
        power_manager_->OnChargingStatusChanged([this](bool charging) { led_->SetCharging(charging); });
        led_->SetCharging(power_manager_->IsCharging());
        if (settings.GetBool("boot_animation", true)) led_->PlayPowerOnEffect();
        DogMedia::GetInstance().SetPlaybackCallback([this](bool playing) {
            if (!playing) return;
            Settings settings("display");
            int posture = settings.GetInt("music_posture", 0);
            pet_dog_.TryAutomaticPosture(posture == 1 ? "sleep" : posture == 2 ? "sitdown" : "stand");
        });

        if (display_) {
            display_->OnSettingChanged([this](const std::string& name, bool enabled) {
                if (name == "free_explore") pet_dog_.SetFreeExplore(enabled);
                if (name == "charging_led") led_->SetChargingEffectEnabled(enabled);
            });
            display_->SetMenuCallbacks([this](const std::string& action) {
                if (action == "reset_network") {
                    RequestNetworkReset();
                    return;
                }
                Application::GetInstance().Schedule([this, action]() {
                    auto& media = DogMedia::GetInstance();
                    std::string result;
                    if (action == "music_toggle") result = media.TogglePlayback();
                    else if (action == "music_next") result = media.Next();
                    else if (action == "music_prev") result = media.Prev();
                    else if (action == "music_stop") result = media.Stop();
                    else if (action == "alarm_toggle") result = media.ToggleAlarm();
                    else if (action == "chat") { media.Stop(); Application::GetInstance().SetChatActive(true); }
                    if (display_ && !result.empty()) display_->ShowNotification(result.c_str());
                });
            }, [](const std::string& page) -> std::string {
                if (page == "music") {
                    auto& media = DogMedia::GetInstance();
                    auto error = media.LastError();
                    if (!error.empty()) return error;
                    auto song = media.CurrentSong();
                    return song.empty() ? "SD卡/music/mp3" :
                        std::string(media.IsPlaying() ? "播放中: " : media.HasPlaybackRequest() ? "待播放: " : "已停止: ") + song;
                }
                if (page == "alarm") return DogMedia::GetInstance().GetAlarm();
                if (page.compare(0, 6, "alarm_") == 0)
                    return DogMedia::GetInstance().GetAlarmField(page.substr(6));
                if (page == "about_version") return std::string("v") + esp_app_get_description()->version;
                if (page == "about_board") return "zfsn-esp32s3-dog";
                if (page == "about_network") {
                    auto& station = GetWifiStation();
                    return station.IsConnected() ? station.GetIpAddress() : "Wi-Fi未连接";
                }
                return SystemInfo::GetUserAgent();
            });
        }

        InitializeVolumeTimers();
        InitializeButtons();
        InitializeWiFi();

        dog_controller_ = new DogController(&pet_dog_, led_);
        dog_controller_->RegisterMcpTools();

        DeviceStateEventManager::GetInstance().RegisterStateChangeCallback([this](DeviceState previous, DeviceState current) {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != current) return;
            if (current == kDeviceStateConnecting || current == kDeviceStateListening || current == kDeviceStateSpeaking) {
                pet_dog_.TryAutomaticPosture("stand");
            } else if (current == kDeviceStateIdle && !DogMedia::GetInstance().IsPlaying() &&
                       !app.GetAudioService().IsLocalPlaybackActive()) {
                pet_dog_.TryAutomaticPosture("sleep");
            }
        });

        ESP_LOGI(TAG, "ZFSN-ESP32S3-DOG initialized");
#ifdef CONFIG_ZFSN_UI_CAPTURE
        if (display_) StartUiCapturePreview();
#endif
    }

    virtual std::string GetBoardType() override {
        return "zfsn-esp32s3-dog";
    }

    void ResetWifiConfiguration() override { RequestNetworkReset(); }

    virtual Led* GetLed() override {
        return led_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        static NoDisplay fallback;
        return display_ ? static_cast<Display*>(display_) : &fallback;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (power_manager_ == nullptr) {
            return false;
        }
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        level = power_manager_->GetBatteryLevel();
        if (led_ && power_manager_->HasBatteryReading()) led_->SetBatteryLevel(level);
        return power_manager_->HasBatteryReading();
    }
};

DECLARE_BOARD(BoardDog);
