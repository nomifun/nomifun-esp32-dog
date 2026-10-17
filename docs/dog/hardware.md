# 硬件与引脚

固件对应原理图中的 v4 贴片版主板。工程文件见 [hardware/](../../hardware/)。

## 组成

| 部件 | 型号 | 说明 |
|---|---|---|
| 主控 | ESP32-S3-WROOM-1U-N16R8 | 16 MB Flash，8 MB PSRAM |
| 屏幕 | 1.3 英寸 SH1106 OLED 128×64 | I2C，地址 0x3C，400 kHz |
| 麦克风 | INMP441 | I2S 输入，16 kHz |
| 功放 | MAX98357A | I2S 输出，24 kHz |
| 舵机 | SG90 180° × 4 | LEDC 50 Hz PWM |
| 灯带 | WS2812 × 8 | RMT 驱动，外接 CN1/CN2 |
| 存储 | microSD | SDMMC 外设，1-bit 模式，挂载点 `/sdcard` |
| 充电 | TP4054 | CHRG 开漏输出，低电平表示正在充电 |
| 电池检测 | 100K/100K 分压 | ADC1_CH9，分压比 2.0 |
| 按键 | BOOT + 3 个功能键 | 外部上拉，低电平有效 |

麦克风和功放使用两组独立的 I2S 时钟，音频驱动为 `NoAudioCodecSimplex`。

## 引脚表

| GPIO | 网络名 | 功能 |
|---|---|---|
| IO4 | `441_WS` | INMP441 WS / LRCLK |
| IO5 | `441_SCK` | INMP441 BCLK |
| IO6 | `441_SD` | INMP441 数据 |
| IO7 | `357_DIN` | MAX98357A 数据 |
| IO15 | `357_BCLK` | MAX98357A BCLK |
| IO16 | `357_LRCLK` | MAX98357A LRCK |
| IO17 | `PWM1_G17` | 舵机 PWM1（左前 LF） |
| IO14 | `PWM2_G14` | 舵机 PWM2（右后 RB） |
| IO13 | `PWM3_G13` | 舵机 PWM3（右前 RF） |
| IO18 | `PWM4_G18` | 舵机 PWM4（左后 LB） |
| IO8 | `LEDC_LEFT_G8` | WS2812 数据 |
| IO41 | `SDA_G41` | I2C SDA（SH1106） |
| IO42 | `SCL_G42` | I2C SCL（SH1106） |
| IO38 | `SD_CMD` | SD 卡 CMD |
| IO39 | `SD_CLK` | SD 卡 CLK |
| IO40 | `SD_DATA` | SD 卡 DAT0 |
| IO0 | `BOOT_G0` | BOOT 键（SW1） |
| IO1 | `KEY_1` | 按键 1（SW2，R14 上拉） |
| IO2 | `KEY_2` | 按键 2（SW3，R15 上拉） |
| IO3 | `KEY_3` | 按键 3（SW4，R16 上拉） |
| IO10 | `BAT_DET` | 电池电压检测（R12/R13 分压） |
| IO47 | `CHG_DET` | TP4054 充电状态 |
| IO19 / IO20 | `USB_DM` / `USB_DP` | USB D- / D+ |

未连接：IO9、IO11、IO12、IO21、IO35、IO36、IO37、IO45、IO46、IO48，以及 RXD0 / TXD0。由于 UART0 未引出，日志使用 USB-Serial-JTAG 输出。

引脚定义在 [`main/boards/zfsn-esp32s3-dog/config.h`](../../main/boards/zfsn-esp32s3-dog/config.h)。

## 舵机映射

| LEDC 通道 | GPIO | 网络 | 腿位 | 角度方向 |
|---|---|---|---|---|
| CH0 | IO14 | PWM2 | 右后 RB | 180 − angle |
| CH1 | IO17 | PWM1 | 左前 LF | angle |
| CH2 | IO13 | PWM3 | 右前 RF | 180 − angle |
| CH3 | IO18 | PWM4 | 左后 LB | angle |

- PWM：50 Hz、13 bit，占空比 0.025–0.13（约 0.5–2.6 ms），对应 0–180°。
- 默认姿态（LF / RF / LB / RB）：站立 90/90/90/90，趴下 180/180/0/0，坐下 90/90/30/30。
- 校准：通过 HTTP `GET /move?dir=save_angles` 保存三种姿态角度，见 [http-api.md](http-api.md)。校准值保存在 NVS（命名空间 `petdog`，键 `angle_settings`），重新配网不会清除。

## SD 卡目录

| 目录 | 用途 |
|---|---|
| `/sdcard/music/mp3/` | 本地音乐（MP3，支持中文长文件名） |
| `/sdcard/custom_action/` | 自定义动作（JSON） |
| `/sdcard/custom_emo/` | 自定义表情（GIF 与同名 JSON 配置） |

## 分区表

[`partitions/v2/zfsn_16m.csv`](../../partitions/v2/zfsn_16m.csv)：

| 名称 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 0x4000 |
| otadata | data/ota | 0xd000 | 0x2000 |
| phy_init | data/phy | 0xf000 | 0x1000 |
| ota_0 | app/ota_0 | 0x20000 | 0x600000 |
| ota_1 | app/ota_1 | 0x620000 | 0x600000 |
| assets | data/spiffs | 0xc20000 | 0x3e0000 |

不要修改偏移和大小：已有设备的 OTA 回退、NVS 数据和资源分区都依赖这份布局。
