#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// Board wiring: IO10/ADC1_CH9, 100K/100K divider (x2),
// IO47/TP4054 CHRG open-drain status (low = charging).
class PowerManager {
public:
    PowerManager(gpio_num_t charging_pin, adc_unit_t adc_unit, adc_channel_t adc_channel);
    ~PowerManager();

    void InitializeAdc();
    void ReadBatteryAdcData();

    bool IsCharging();
    bool IsDischarging();
    bool IsLowBattery();
    uint32_t GetBatteryLevel();
    uint32_t GetBatteryVoltageMv();
    bool HasBatteryReading();

    void OnChargingStatusChanged(std::function<void(bool)> callback);
    void OnLowBatteryStatusChanged(std::function<void(bool)> callback);

private:
    std::mutex state_mutex_;
    std::mutex adc_mutex_;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;

    gpio_num_t charging_pin_;
    adc_unit_t adc_unit_;
    adc_channel_t adc_channel_;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t calibration_handle_ = nullptr;
    std::vector<uint16_t> voltage_samples_;

    uint32_t battery_level_ = 0;
    uint32_t battery_voltage_mv_ = 0;
    bool has_battery_reading_ = false;
    bool is_charging_ = false;
    bool is_low_battery_ = false;
    bool stopping_ = false;
    uint32_t ticks_ = 0;
    TaskHandle_t task_handle_ = nullptr;
    SemaphoreHandle_t stopped_ = nullptr;

    static constexpr uint32_t kBatteryAdcInterval = 60;
    static constexpr size_t kBatteryAdcDataCount = 3;
    static constexpr uint32_t kLowBatteryLevel = 20;

    void Run();
    void CheckBatteryStatus();
};
