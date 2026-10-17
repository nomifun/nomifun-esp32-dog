# MCP 工具

设备通过 MCP 向小智服务端注册以下工具，大模型据此控制机器狗。工具的描述原文与参数定义见
[`board_dog.cc`](../../main/boards/zfsn-esp32s3-dog/board_dog.cc) 中的 `DogController::RegisterMcpTools()`。
参数名区分大小写，整数参数必须是范围内的有限整数。

## 板级工具

| 工具 | 参数 | 说明 |
|---|---|---|
| `self.dog.action` | `action`；`steps` 1–100（默认 3）；`angle` 1–360（默认 30） | 动作：`walk_forward` `walk_back` `turn_left` `turn_right` `sitdown` `stand` `sleep` `stretch` `scratching` `swing` `petwave` `stop` `jump`。`steps` 只用于前进/后退，`angle` 只用于转向。返回成功表示动作已接受，实际动作异步执行；新命令会取代正在执行的命令 |
| `self.dog.setting` | `setting`：`charging_led` / `boot_animation` / `free_explore` / `all`；`action`：`get`（默认）/ `set` / `toggle`；`enabled` | 查询或修改设置，返回全部三项当前值。`all` 只能查询；`set` 时必须提供 `enabled` |
| `self.ws2812_led.control` | `mode`：`turn_on_off` / `flash_light` / `water_light` / `breathe_light` / `rainbow_light`；`color`：红灯、橙灯、黄灯、绿灯、蓝灯、紫灯、白灯、关灯（默认关灯）；`brightness` 0–100（默认 50） | 灯光控制 |
| `self.display.set_emotion` | `emotion` | 显示 21 种内置表情之一，列表见 [http-api.md](http-api.md#表情get-emotion) |
| `self.display.firework` | 无 | 播放烟花动画 |
| `self.music.play` | `song_name`（必填）；`artist`（可选） | 在 SD 卡 `music/mp3` 中模糊搜索并播放 |
| `self.recommended_music.get_recommended_music` | 无 | 从 SD 卡随机推荐 5 首歌 |
| `self.alarm.set_alarm` | `year` `month` `day`（可选，默认今天）；`hour` 0–23；`minute` 0–59；`song_name`（必填）；`artist`；`enabled`（默认 true） | 设置一次性闹钟，到点播放匹配到的本地歌曲，返回实际匹配的文件名 |
| `self.alarm.get_alarm` | 无 | 查询闹钟的日期时间、歌曲和启用状态 |

## 通用工具（来自上游）

| 工具 | 说明 |
|---|---|
| `self.get_device_status` | 设备状态（音量、网络、电量等） |
| `self.audio_speaker.set_volume` | 设置音量 0–100 |

本板没有背光和摄像头，因此不会注册 `self.screen.set_brightness` 与 `self.camera.take_photo`。
