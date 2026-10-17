# dog_host 离线主机回归测试

在 Windows 上以主机替代（host substitute）方式编译并运行固件中的真实 C/C++
代码。覆盖范围：

- **board_buttons**：`board_dog.cc` 的按键/音量方法与定时器回调派发。
- **board_menu**：`SetMenuCallbacks` 菜单回调与 `Application` 聊天入口。
- **chat_control**：`application.cc` 聊天状态/调度/中止等方法与 TTS 回调。
- **control**：MCP `DoToolCall` 分发、Web 控制端点与 `/saved/delete` 处理器。
- **display**：`display_board.cc` + `oled_display.cc` + LVGL 软件渲染契约
  （输出为软件预览，不是真机截图）。
- **http_lifetime**：`78__esp-ml307` 的 HTTP/WebSocket/TCP/TLS 生命周期。
- **media**：`dog_media.cc` 播放/闹钟/NVS 持久化行为。
- **network_reset**：`network_reset.cc` + `settings.cc` 配网重置持久化。
- **ota_setup**：`Application::CheckNewVersion` 与启动 OTA 门控。
- **pet_dog**：`pet_dog.cc` 动作仲裁/校准/SD 自定义动作。
- **panel_capture**：`panel_capture.cc` 帧捕获协议与 Python 解码器。
- **wifi-***：`components/78__esp-wifi-connect` 的 saved_add、SSID 持久化、
  provisioning 存储/HTTP/连接、station 与 board 恢复测试。
- **tools-contracts**：`tools/` 目录的离线 Python 单元测试。

## 先决条件

- Windows + Visual Studio Build Tools（含 MSVC x64 与 Windows SDK）。
- Node.js（用于 `run.mjs` 各套件）。
- Python 3（`run_all.py` 与 Python 套件）。
- 环境变量 `IDF_PATH` 指向已安装的 ESP-IDF（仅用于
  `components/json/cJSON` 源码，不调用 ESP-IDF 构建）。
- 无需设备、串口、网络访问或软件包下载。

## 运行

全部套件（结果写入 `build/dog_host/<label>/`）：

```powershell
python tests/dog_host/run_all.py --label final
```

单个套件示例：

```powershell
node tests/dog_host/pet_dog_host/run.mjs
node tests/dog_host/media_host/run.mjs
python tests/dog_host/board_buttons_host/run.py
python tests/dog_host/run_display_host_tests.py --phase after
python tests/dog_host/run_panel_capture_host_tests.py
python components/78__esp-wifi-connect/tests/test_saved_add.py
```

每个套件在 `build/dog_host/<suite>/` 下写构建日志、测试日志与
`result.json`；`run_all.py` 另在 `build/dog_host/<label>/result.json`
写聚合回执，含运行前后的源码 SHA-256。

## 边界

全部通过只代表主机替代环境下的行为契约；不构成真机验证。ESP32 时序、
OLED 实物、Wi-Fi/网络重连、SD/FatFS、伺服运动与 OTA 流程仍需设备验收。
