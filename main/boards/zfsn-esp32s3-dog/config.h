#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>

// ===== Audio (INMP441 + MAX98357A, NoAudioCodecSimplex) =====
// Schematic v4: 441_WS/SCK/SD + 357_BCLK/DIN/LRCLK
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

// ===== Buttons (SW1..SW4, pull-up to VCC_3V3, active low) =====
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define BUTTON_1_GPIO           GPIO_NUM_1
#define BUTTON_2_GPIO           GPIO_NUM_2
#define BUTTON_3_GPIO           GPIO_NUM_3

// ===== WS2812 LED strip (net LEDC_LEFT_G8 -> CN1/CN2 external strip) =====
#define WS2812_LED_GPIO         GPIO_NUM_8
// WS2812LED on GPIO8, 8 pixels.
#define WS2812_LED_COUNT        8

// ===== Power management =====
// BAT_DET -> IO10 (ADC1_CH9), R12/R13 = 100K/100K divider (x2)
// CHG_DET -> IO47, TP4054 CHRG open-drain (low = charging)
#define POWER_CHARGE_DETECT_PIN GPIO_NUM_47
#define POWER_ADC_UNIT          ADC_UNIT_1
#define POWER_ADC_CHANNEL       ADC_CHANNEL_9

// ===== SH1106 OLED (I2C, 1.3" 128x64) =====
#define DISPLAY_SDA_PIN         GPIO_NUM_41
#define DISPLAY_SCL_PIN         GPIO_NUM_42
#define DISPLAY_WIDTH           128
#define DISPLAY_HEIGHT          64
// Both mirrors stay off for this board's OLED mounting; enabling both would
// rotate the image by 180 deg.
#define DISPLAY_MIRROR_X        false
#define DISPLAY_MIRROR_Y        false

// ===== SD card (1-bit SDMMC, nets SD_CLK/SD_CMD/SD_DATA) =====
#define SDCARD_SDMMC_CLK_PIN    GPIO_NUM_39
#define SDCARD_SDMMC_CMD_PIN    GPIO_NUM_38
#define SDCARD_SDMMC_D0_PIN     GPIO_NUM_40
#define SDCARD_SDMMC_BUS_WIDTH  1
#define SDCARD_MOUNT_POINT      "/sdcard"

// ===== Servos (SG90 180deg, LEDC 50Hz) =====
// Schematic nets: PWM1_G17 PWM2_G14 PWM3_G13 PWM4_G18.
// Channel mapping: CH0=G14->RB, CH1=G17->LF, CH2=G13->RF, CH3=G18->LB.
// InitializeDog(14,17,13,18).
#define SERVO_PWM1_GPIO         GPIO_NUM_17   // LF
#define SERVO_PWM2_GPIO         GPIO_NUM_14   // RB
#define SERVO_PWM3_GPIO         GPIO_NUM_13   // RF
#define SERVO_PWM4_GPIO         GPIO_NUM_18   // LB

#endif // _BOARD_CONFIG_H_
