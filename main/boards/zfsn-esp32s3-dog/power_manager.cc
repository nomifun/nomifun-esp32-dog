#include "power_manager.h"

#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <utility>

#define TAG "PowerManager"

PowerManager::PowerManager(gpio_num_t charging_pin, adc_unit_t adc_unit, adc_channel_t adc_channel)
    : charging_pin_(charging_pin), adc_unit_(adc_unit), adc_channel_(adc_channel) {
    if (charging_pin_ != GPIO_NUM_NC) {
        gpio_config_t io_conf = {};
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = 1ULL << charging_pin_;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));
        is_charging_ = gpio_get_level(charging_pin_) == 0;
    }
    InitializeAdc();
    stopped_ = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(stopped_ ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate([](void* arg) {
        static_cast<PowerManager*>(arg)->Run();
    }, "battery_check", 3072, this, 3, &task_handle_) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

PowerManager::~PowerManager() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        stopping_ = true;
    }
    if (task_handle_) {
        xTaskNotifyGive(task_handle_);
        xSemaphoreTake(stopped_, portMAX_DELAY);
    }
    if (stopped_) vSemaphoreDelete(stopped_);
    std::lock_guard<std::mutex> lock(adc_mutex_);
    if (calibration_handle_) adc_cali_delete_scheme_curve_fitting(calibration_handle_);
    if (adc_handle_) adc_oneshot_del_unit(adc_handle_);
}

void PowerManager::InitializeAdc() {
    std::lock_guard<std::mutex> lock(adc_mutex_);
    if (adc_handle_) return;
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = adc_unit_;
    init_config.ulp_mode = ADC_ULP_MODE_DISABLE;
    esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC initialization failed: %s", esp_err_to_name(err));
        return;
    }
    adc_oneshot_chan_cfg_t channel_config = {};
    channel_config.atten = ADC_ATTEN_DB_12;
    channel_config.bitwidth = ADC_BITWIDTH_12;
    err = adc_oneshot_config_channel(adc_handle_, adc_channel_, &channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel initialization failed: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(adc_handle_);
        adc_handle_ = nullptr;
        return;
    }
    // ESP32-S3 curve fitting uses the chip's factory eFuse calibration data.
    adc_cali_curve_fitting_config_t calibration_config = {};
    calibration_config.unit_id = adc_unit_;
    calibration_config.chan = adc_channel_;
    calibration_config.atten = channel_config.atten;
    calibration_config.bitwidth = channel_config.bitwidth;
    err = adc_cali_create_scheme_curve_fitting(&calibration_config, &calibration_handle_);
    if (err != ESP_OK) {
        // Do not invent a full-scale voltage or report raw counts as millivolts.
        ESP_LOGE(TAG, "Battery calibration unavailable: %s", esp_err_to_name(err));
    }
}

void PowerManager::Run() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (stopping_) break;
        }
        CheckBatteryStatus();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
    xSemaphoreGive(stopped_);
    vTaskDelete(nullptr);
}

void PowerManager::CheckBatteryStatus() {
    const bool charging = charging_pin_ != GPIO_NUM_NC && gpio_get_level(charging_pin_) == 0;
    std::function<void(bool)> callback;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        changed = charging != is_charging_;
        if (changed) {
            is_charging_ = charging;
            callback = on_charging_status_changed_;
        }
    }
    // Callbacks may query this object or update LED state; never hold its locks.
    if (callback) callback(charging);

    bool warming_up;
    {
        std::lock_guard<std::mutex> lock(adc_mutex_);
        warming_up = voltage_samples_.size() < kBatteryAdcDataCount;
    }
    if (changed || warming_up || ++ticks_ >= kBatteryAdcInterval) {
        ticks_ = 0;
        ReadBatteryAdcData();
    }
}

void PowerManager::ReadBatteryAdcData() {
    uint32_t voltage_mv = 0;
    {
        std::lock_guard<std::mutex> lock(adc_mutex_);
        if (!adc_handle_ || !calibration_handle_) return;
        int raw = 0;
        esp_err_t err = adc_oneshot_read(adc_handle_, adc_channel_, &raw);
        int divider_mv = 0;
        if (err == ESP_OK) err = adc_cali_raw_to_voltage(calibration_handle_, raw, &divider_mv);
        if (err != ESP_OK || divider_mv <= 0) {
            // Keep the last valid sample; transient ADC/RF contention must not reboot.
            ESP_LOGW(TAG, "Battery read failed: %s", esp_err_to_name(err));
            return;
        }
        voltage_samples_.push_back(static_cast<uint16_t>(divider_mv * 2));
        if (voltage_samples_.size() > kBatteryAdcDataCount) voltage_samples_.erase(voltage_samples_.begin());
        for (uint16_t sample : voltage_samples_) voltage_mv += sample;
        voltage_mv /= voltage_samples_.size();
    }

    // Approximate single-cell LiPo curve.
    // Divider calibration fixes ADC voltage, but percentage still needs bench validation.
    const struct { uint16_t mv; uint8_t percent; } levels[] = {
        {3200, 0}, {3400, 20}, {3600, 40}, {3800, 60}, {4000, 80}, {4200, 100},
    };
    uint32_t level = 0;
    for (const auto& entry : levels) if (voltage_mv >= entry.mv) level = entry.percent;
    const bool low = level <= kLowBatteryLevel;
    std::function<void(bool)> callback;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        battery_voltage_mv_ = voltage_mv;
        battery_level_ = level;
        has_battery_reading_ = true;
        if (is_low_battery_ != low) {
            is_low_battery_ = low;
            callback = on_low_battery_status_changed_;
        }
    }
    if (callback) callback(low);
}

bool PowerManager::IsCharging() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return is_charging_;
}

bool PowerManager::IsDischarging() { return !IsCharging(); }

bool PowerManager::IsLowBattery() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return is_low_battery_;
}

uint32_t PowerManager::GetBatteryLevel() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return battery_level_;
}

uint32_t PowerManager::GetBatteryVoltageMv() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return battery_voltage_mv_;
}

bool PowerManager::HasBatteryReading() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return has_battery_reading_;
}

void PowerManager::OnChargingStatusChanged(std::function<void(bool)> callback) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    on_charging_status_changed_ = std::move(callback);
}

void PowerManager::OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    on_low_battery_status_changed_ = std::move(callback);
}
