#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <led_strip.h>

#include "led/led.h"

// WS2812 strip control: five public modes plus charging and power-on tasks.
// Frame timing is approximate; this implementation serializes effects in one
// task instead of racing RMT tasks.
class WS2812LED : public Led {
public:
    enum class Mode { kOff, kTurnOnOff, kFlash, kWater, kBreathe, kRainbow };

    WS2812LED(gpio_num_t pin, uint32_t count);
    ~WS2812LED();
    void OnStateChanged() override;

    void Control(Mode mode, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);
    void Control(const std::string& mode, const std::string& color, int brightness);
    void SetChargingEffectEnabled(bool enabled);
    void SetCharging(bool charging);
    void SetBatteryLevel(uint32_t level);
    void PlayPowerOnEffect();
    // Kept for compatibility: this setting controls automatic battery effects
    // only. User Control is always available.
    void SetEnabled(bool enabled) { SetChargingEffectEnabled(enabled); }

private:
    enum class Source { kUser, kCharging, kFull, kLowBattery, kPowerOn };
    struct Frame {
        Source source;
        Mode mode;
        uint8_t r, g, b, brightness;
        uint32_t number;
    };

    led_strip_handle_t strip_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    SemaphoreHandle_t stopped_ = nullptr;
    std::mutex mutex_;
    const uint32_t count_;
    Mode mode_ = Mode::kOff;
    uint8_t r_ = 255, g_ = 255, b_ = 255;
    uint8_t brightness_ = 50;
    uint32_t frame_ = 0;
    uint32_t power_on_frame_ = 0;
    uint32_t battery_level_ = 0;
    bool battery_level_known_ = false;
    bool charging_effect_enabled_ = true;
    bool charging_ = false;
    bool automatic_suppressed_ = false;
    bool power_on_active_ = false;
    bool stopping_ = false;

    void Run();
    void Render(const Frame& frame);
    void WriteAll(uint8_t r, uint8_t g, uint8_t b);
    void WritePixel(uint32_t index, uint8_t r, uint8_t g, uint8_t b);
    static void HsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b);
    static bool ModeFromName(const std::string& name, Mode& mode);
    static void ColorFromName(const std::string& name, uint8_t& r, uint8_t& g, uint8_t& b);
};
