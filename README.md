# nomifun-esp32-dog

基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) v2.0.5 的 ESP32-S3 四足桌面机器狗固件（板型 `zfsn-esp32s3-dog`）。
语音对话由小智服务端（WebSocket / MQTT+UDP）完成，大模型通过 MCP 工具控制机器狗的动作、表情、灯光、音乐和闹钟；
在局域网内也可以通过 HTTP 接口直接控制。

## 功能

- **语音交互**：离线唤醒词“你好小智”（ESP-SR WakeNet），流式 ASR + LLM + TTS。
- **动作**：4 路 SG90 舵机，支持前进、后退、左转、右转、坐下、站立、趴下、伸懒腰、挠痒痒、摇摆、挥手、跳跃。
  对话状态会自动切换姿态；开启“自由探索”后空闲时随机做动作；支持 SD 卡自定义动作；舵机角度可校准并保存。
- **显示**：1.3 英寸 SH1106 128×64 OLED，提供 21 种表情、烟花动画、横向像素菜单，并支持 SD 卡自定义 GIF 表情。
- **灯光**：8 颗 WS2812，支持开关灯、闪烁、流水、呼吸、彩虹灯效，以及开机灯效和充电灯效。
- **媒体**：播放 SD 卡 `music/mp3` 中的本地音乐（模糊搜索、推荐、上一首/下一首），可设置一次性日期闹钟播放指定歌曲。
- **电源**：检测锂电池电压和 TP4054 充电状态。
- **网络**：配网热点同页填写 Wi-Fi 与 OTA 地址，连接失败时显示原因；BOOT 长按 5 秒可重新配网；
  提供局域网 HTTP 控制接口和本地固件升级页面。

## 硬件

| 部件 | 型号 / 连接 |
|---|---|
| 主控 | ESP32-S3-WROOM-1U-N16R8（16 MB Flash，8 MB PSRAM） |
| 屏幕 | 1.3 英寸 SH1106 OLED 128×64，I2C（SDA IO41 / SCL IO42） |
| 麦克风 | INMP441（I2S，IO4/5/6） |
| 功放 | MAX98357A（I2S，IO7/15/16） |
| 舵机 | 4 × SG90 180°（IO17 / IO14 / IO13 / IO18） |
| 灯带 | 8 × WS2812（IO8） |
| 存储 | microSD，1-bit SDMMC（CLK IO39 / CMD IO38 / D0 IO40） |
| 按键 | BOOT（IO0）+ 3 个功能键（IO1 / IO2 / IO3） |
| 电源 | 锂电池，TP4054 充电（状态 IO47），电池分压检测（IO10） |

完整引脚、舵机映射和分区表见 [docs/dog/hardware.md](docs/dog/hardware.md)，原理图与 PCB 工程见 [hardware/](hardware/)。

## 快速开始

### 1. 构建

需要 ESP-IDF 5.4 及以上的 5.x 版本（已在 v5.5.5 上验证）。

```sh
git clone <本仓库地址> nomifun-esp32-dog
cd nomifun-esp32-dog
idf.py set-target esp32s3
idf.py build
```

`sdkconfig.defaults.esp32s3` 已默认选择本板型，无需在 menuconfig 中切换。
固件的 CMake 工程名保持为 `xiaozhi`，产物为 `build/xiaozhi.bin`。设备会把工程名作为应用名上报给 OTA 服务端，请不要修改。

### 2. 烧录

首次刷写前请先备份整片 Flash，以便恢复出厂固件。之后执行：

```sh
idf.py -p <串口> flash monitor
```

备份、恢复和只更新应用分区等操作见 [docs/dog/build-and-flash.md](docs/dog/build-and-flash.md)。

### 3. 配网与使用

1. 首次启动后设备开启热点 `Xiaozhi-XXXX`。用手机连接后打开 `http://192.168.4.1`，选择 Wi-Fi 并输入密码（可同时填写 OTA 地址）。
2. 设备重启并联网。如果 60 秒内未获得 IP，会重新开启热点，并在屏幕上显示失败原因。
3. 按屏幕或语音提示，在小智控制台绑定设备。
4. 说“你好小智”或单击 BOOT 键开始对话。按键与菜单说明见 [docs/dog/menu-guide.md](docs/dog/menu-guide.md)。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/dog/hardware.md](docs/dog/hardware.md) | 硬件组成、引脚、舵机映射、分区表 |
| [docs/dog/build-and-flash.md](docs/dog/build-and-flash.md) | 构建、烧录、备份与恢复、诊断构建 |
| [docs/dog/menu-guide.md](docs/dog/menu-guide.md) | 按键与屏幕菜单 |
| [docs/dog/mcp-tools.md](docs/dog/mcp-tools.md) | 提供给大模型的 MCP 工具 |
| [docs/dog/http-api.md](docs/dog/http-api.md) | 局域网 HTTP 控制接口 |
| [tests/dog_host/README.md](tests/dog_host/README.md) | Windows 主机测试 |
| [tools/README.md](tools/README.md) | 设备备份、刷写与串口采集工具 |

通信协议和自定义开发板等通用说明见上游文档：[WebSocket](docs/websocket.md)、[MQTT+UDP](docs/mqtt-udp.md)、
[MCP 协议](docs/mcp-protocol.md)、[自定义开发板](docs/custom-board.md)。

## 目录结构

```text
main/boards/zfsn-esp32s3-dog/     机器狗板级代码
├── board_dog.cc                  板级初始化、按键、MCP 工具注册
├── pet_dog.*                     舵机驱动与动作调度
├── display_board.*               OLED 表情与菜单
├── dog_media.*                   SD 卡音乐与闹钟
├── web_control_server.*          局域网 HTTP 控制
├── ws2812_LED.*                  灯带效果
├── power_manager.*               电池与充电检测
├── network_reset.*               重置网络设置
├── panel_capture.*               OLED 帧采集（诊断构建）
├── pixel_icons.h                 菜单像素图标
└── assets/dog_assets.bin         预构建资源镜像（字体、表情、唤醒词模型）
components/78__esp-wifi-connect/  配网组件（本地修改版）
components/78__esp-ml307/         网络、HTTP、WebSocket 组件（本地修改版）
partitions/v2/zfsn_16m.csv        分区表
docs/dog/                         机器狗文档
hardware/                         原理图与 PCB 工程
tests/dog_host/                   主机测试
tools/                            设备工具
```

## 相对上游 v2.0.5 的主要改动

- 新增板型 `zfsn-esp32s3-dog` 及其构建配置：16 MB 分区表、DIO Flash、USB-Serial-JTAG 控制台、FATFS 长文件名。
- 启动时 OTA 检查有次数上限，失败后提示 BOOT 恢复入口。新增幂等的对话唤醒/休眠接口。
- 下行音频在主循环中有序排队；本地音乐播放与对话音频互相仲裁；AudioService 启停会等待旧任务退出。
- MCP：错误信息用 cJSON 生成，参数校验更严格，并输出调用日志。
- Wi-Fi：连接失败原因显示在屏幕和配网页上。Settings 删除键后会正确提交。
- 组件：esp-wifi-connect 3.1.5 增加配网页保存按钮与 `POST /saved/add`；esp-ml307 3.5.3 修复了 HTTP/TCP/TLS/WebSocket 关闭与重连时的生命周期问题。

## 许可

- 本项目以 [MIT 许可](LICENSE) 开源。项目基于同为 MIT 许可的 xiaozhi-esp32 开发，LICENSE 中保留了其版权声明。`components/` 下的组件遵循各自的 LICENSE。
- `dog_assets.bin` 中的字体、表情图片和唤醒词模型属于第三方资源，遵循各自的许可，不在本仓库 MIT 许可范围内。
- 原理图与 PCB 的版权与许可以硬件原作者的声明为准，见 [hardware/README.md](hardware/README.md)。

## 致谢

- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)、[78/esp-wifi-connect](https://github.com/78/esp-wifi-connect)、[78/esp-ml307](https://github.com/78/esp-ml307)
- [Espressif ESP-IDF](https://github.com/espressif/esp-idf)、[ESP-SR](https://github.com/espressif/esp-sr)
