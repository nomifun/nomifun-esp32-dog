#include "ws2812_LED.h"

#include <algorithm>
#include <esp_log.h>

#define TAG "WS2812LED"

WS2812LED::WS2812LED(gpio_num_t pin, uint32_t count) : count_(count) {
    ESP_ERROR_CHECK(count_ > 0 ? ESP_OK : ESP_ERR_INVALID_ARG);
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = pin;
    strip_config.max_leds = count_;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;
    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_));
    stopped_ = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(stopped_ ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate([](void* arg) {
        static_cast<WS2812LED*>(arg)->Run();
    }, "ws2812_effect", 3072, this, 3, &task_handle_) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

WS2812LED::~WS2812LED() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    if (task_handle_) {
        xTaskNotifyGive(task_handle_);
        xSemaphoreTake(stopped_, portMAX_DELAY);
    }
    if (stopped_) vSemaphoreDelete(stopped_);
    if (strip_) led_strip_del(strip_);
}

void WS2812LED::SetChargingEffectEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (charging_effect_enabled_ == enabled) return;
        charging_effect_enabled_ = enabled;
        automatic_suppressed_ = false;
        frame_ = 0;
    }
    xTaskNotifyGive(task_handle_);
}

void WS2812LED::SetCharging(bool charging) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (charging_ == charging) return;
        charging_ = charging;
        automatic_suppressed_ = false;
        frame_ = 0;
    }
    xTaskNotifyGive(task_handle_);
}

void WS2812LED::SetBatteryLevel(uint32_t level) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        battery_level_ = std::min(level, uint32_t{100});
        battery_level_known_ = true;
    }
    xTaskNotifyGive(task_handle_);
}

void WS2812LED::PlayPowerOnEffect() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        power_on_active_ = true;
        automatic_suppressed_ = false;
        power_on_frame_ = 0;
    }
    xTaskNotifyGive(task_handle_);
}

void WS2812LED::Control(Mode mode, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = mode;
        r_ = r; g_ = g; b_ = b;
        brightness_ = std::min(brightness, uint8_t{100});
        frame_ = 0;
        power_on_active_ = false;
        // Explicit user commands take precedence until a charging state/setting
        // change. Turning charging_led off must never disable these commands.
        automatic_suppressed_ = true;
    }
    xTaskNotifyGive(task_handle_);
}

void WS2812LED::Control(const std::string& mode, const std::string& color, int brightness) {
    Mode parsed;
    if (!ModeFromName(mode, parsed)) {
        ESP_LOGW(TAG, "Unknown led mode: %s", mode.c_str());
        return;
    }
    uint8_t r, g, b;
    if (parsed == Mode::kTurnOnOff || parsed == Mode::kOff) {
        ColorFromName(color, r, g, b);
    } else {
        // Color/brightness apply to turn_on_off only.
        // Retain the selected color for animated modes; use white after 'off'.
        std::lock_guard<std::mutex> lock(mutex_);
        r = r_; g = g_; b = b_;
        if (r == 0 && g == 0 && b == 0) r = g = b = 255;
        brightness = brightness_;
        if (brightness == 0) brightness = 50;
    }
    Control(parsed, r, g, b, std::clamp(brightness, 0, 100));
}

void WS2812LED::Run() {
    while (true) {
        Frame frame;
        bool animated;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) break;
            // Power-on sequence: ten purple sweeps, a warm sequence,
            // then three white flashes. Detailed timing is an approximation.
            if (power_on_active_ && power_on_frame_ >= count_ * 14 + 100) {
                power_on_active_ = false;
                frame_ = 0;
            }
            Source source = Source::kUser;
            if (power_on_active_) source = Source::kPowerOn;
            else if (charging_effect_enabled_ && !automatic_suppressed_) {
                if (charging_) source = battery_level_known_ && battery_level_ >= 100 ? Source::kFull : Source::kCharging;
                else if (battery_level_known_ && battery_level_ < 20) source = Source::kLowBattery;
            }
            frame = {source, mode_, r_, g_, b_, brightness_,
                source == Source::kPowerOn ? power_on_frame_++ : frame_++};
            animated = source == Source::kPowerOn || source == Source::kCharging || source == Source::kLowBattery ||
                (source == Source::kUser && mode_ != Mode::kOff && mode_ != Mode::kTurnOnOff);
        }
        Render(frame);
        // All pixel writes and RMT waits run here, never on the shared esp_timer task.
        ulTaskNotifyTake(pdTRUE, animated ? pdMS_TO_TICKS(20) : portMAX_DELAY);
    }
    led_strip_clear(strip_);
    xSemaphoreGive(stopped_);
    vTaskDelete(nullptr);
}

void WS2812LED::WritePixel(uint32_t index, uint8_t r, uint8_t g, uint8_t b) {
    led_strip_set_pixel(strip_, index % count_, r, g, b);
}

void WS2812LED::WriteAll(uint8_t r, uint8_t g, uint8_t b) {
    for (uint32_t i = 0; i < count_; ++i) WritePixel(i, r, g, b);
}

void WS2812LED::Render(const Frame& frame) {
    WriteAll(0, 0, 0);
    const uint32_t number = frame.number;
    if (frame.source == Source::kCharging) {
        // Two adjacent green pixels, level 10.
        const uint32_t head = number / 10;
        WritePixel(head, 0, 10, 0);
        WritePixel(head + 1, 0, 10, 0);
    } else if (frame.source == Source::kFull) {
        // Charging + 100 percent -> red, level 10.
        WriteAll(10, 0, 0);
    } else if (frame.source == Source::kLowBattery) {
        // Short red flash below 20 percent.
        if (number % 50 < 10) WriteAll(50, 0, 0);
    } else if (frame.source == Source::kPowerOn) {
        if (number < count_ * 10) {
            WritePixel(number, 10, 0, 20);
        } else if (number < count_ * 14) {
            const uint32_t lit = (number - count_ * 10) / 4;
            for (uint32_t i = 0; i <= lit && i < count_; ++i) WritePixel(i, 60, 20, 0);
        } else {
            const uint32_t tail = number - count_ * 14;
            if (tail < 20) {
                const uint8_t value = tail * 5;
                WriteAll(value, value / 2, 0);
            } else if (tail >= 40 && (tail - 40) % 20 < 10) {
                WriteAll(127, 127, 127);
            }
        }
    } else {
        const uint8_t r = uint32_t(frame.r) * frame.brightness / 100;
        const uint8_t g = uint32_t(frame.g) * frame.brightness / 100;
        const uint8_t b = uint32_t(frame.b) * frame.brightness / 100;
        switch (frame.mode) {
        case Mode::kTurnOnOff:
            WriteAll(r, g, b);
            break;
        case Mode::kFlash:
            // Alternating all-off/all-on frames.
            if (number % 10 >= 5) WriteAll(r, g, b);
            break;
        case Mode::kWater: {
            // Two adjacent pixels flowing around the strip.
            const uint32_t head = number / 10;
            WritePixel(head, r, g, b);
            WritePixel(head + 1, r, g, b);
            break;
        }
        case Mode::kBreathe: {
            // Linear rise and fall, resampled to 20 ms.
            const uint32_t phase = number % 100;
            const uint32_t level = phase < 50 ? phase * 2 : (100 - phase) * 2;
            WriteAll(uint32_t(r) * level / 100, uint32_t(g) * level / 100, uint32_t(b) * level / 100);
            break;
        }
        case Mode::kRainbow:
            for (uint32_t i = 0; i < count_; ++i) {
                uint8_t red, green, blue;
                HsvToRgb((number + i * 360 / count_) % 360, 255, uint32_t(frame.brightness) * 255 / 100,
                    red, green, blue);
                WritePixel(i, red, green, blue);
            }
            break;
        case Mode::kOff:
            break;
        }
    }
    esp_err_t err = led_strip_refresh(strip_);
    if (err != ESP_OK) ESP_LOGW(TAG, "LED refresh failed: %s", esp_err_to_name(err));
}

void WS2812LED::OnStateChanged() {
    // Chat-state notifications do not overwrite user-selected or battery effects.
}

void WS2812LED::HsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint8_t region = h / 60;
    const uint16_t remainder = (h - region * 60) * 255 / 60;
    const uint8_t p = v * (255 - s) / 255;
    const uint8_t q = v * (255 - (s * remainder) / 255) / 255;
    const uint8_t t = v * (255 - (s * (255 - remainder)) / 255) / 255;
    switch (region) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

bool WS2812LED::ModeFromName(const std::string& name, Mode& mode) {
    if (name == "turn_on_off") mode = Mode::kTurnOnOff;
    else if (name == "flash_light") mode = Mode::kFlash;
    else if (name == "water_light") mode = Mode::kWater;
    else if (name == "breathe_light") mode = Mode::kBreathe;
    else if (name == "rainbow_light") mode = Mode::kRainbow;
    else if (name == "off") mode = Mode::kOff;
    else return false;
    return true;
}

void WS2812LED::ColorFromName(const std::string& name, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (name == "red" || name == "红灯") { r = 255; g = 0; b = 0; }
    else if (name == "orange" || name == "橙灯") { r = 255; g = 128; b = 0; }
    else if (name == "yellow" || name == "黄灯") { r = 255; g = 255; b = 0; }
    else if (name == "green" || name == "绿灯") { r = 0; g = 255; b = 0; }
    else if (name == "blue" || name == "蓝灯") { r = 0; g = 0; b = 255; }
    else if (name == "purple" || name == "紫灯") { r = 128; g = 0; b = 255; }
    else if (name == "white" || name == "白灯") { r = 255; g = 255; b = 255; }
    else { r = 0; g = 0; b = 0; }
}
