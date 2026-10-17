# 局域网 HTTP 接口

设备连上 Wi-Fi 后，会在 80 端口启动控制服务（配网热点模式下不启动）。以下示例中的 `http://<设备IP>` 可以在菜单“关于”页查看。
实现位于 [`web_control_server.cc`](../../main/boards/zfsn-esp32s3-dog/web_control_server.cc)。

## 约定

- 所有路由都返回 CORS 头：`Access-Control-Allow-Origin: *`，允许 `GET, POST, DELETE, HEAD, OPTIONS`，允许的请求头为 `Content-Type, X-Sd-Path`。`OPTIONS` 预检直接返回 200。
- 查询参数需要 URL 编码，不能包含控制字符，同一个参数不能重复出现，查询串最长 12288 字节。
- 布尔查询参数（如 `list`、`load`、`delete`）只接受 `1` 或 `true`。
- 文件名不能为空，最长 128 字节，不能是 `.` / `..`，也不能包含 `/ \ : * ? " < > |` 或换行。
- 成功时一般返回纯文本 `OK`（JSON 接口另有说明）。参数错误返回 400，资源不存在返回 404，方法不允许返回 405，依赖不可用（如显示、音频、SD 卡）返回 503。

## 运动、姿态与灯光：`GET /move`

`/remote_action` 与 `/move` 等价。动作和灯光参数可以出现在同一个请求中；服务端会先校验完整请求，再执行动作。

| 参数 | 取值 | 说明 |
|---|---|---|
| `dir` | `forward` / `back` / `left` / `right` / `stop` | 前进、后退、左转、右转、停止 |
| `dir` | `sitdown` / `stand` / `sleep` / `stretch` / `scratching` / `swing` / `petwave` / `jump` | 固定动作。`sleep` 会同时结束对话 |
| `dir` | `wake` | 唤醒对话并站立；重复唤醒会保持当前会话 |
| `dir` | `save_angles` | 保存舵机校准，见下文 |
| `dir` | `light_effect` | 与 `effect` 配合使用（兼容写法） |
| `color` | `red` `orange` `yellow` `green` `blue` `purple` `white` `off`，或 `红灯` `橙灯` `黄灯` `绿灯` `蓝灯` `紫灯` `白灯` `关灯`，或 `RGB(r,g,b)` | 灯光颜色 |
| `brightness` | 0–100，默认 50 | 灯光亮度 |
| `effect` | `flash_light` / `water_light` / `breathe_light` / `rainbow_light` | 闪烁 / 流水 / 呼吸 / 彩虹 |

```sh
curl "http://<设备IP>/move?dir=forward"
curl "http://<设备IP>/move?dir=sitdown"
curl "http://<设备IP>/move?color=RGB(255,128,0)&brightness=50"
curl "http://<设备IP>/move?effect=rainbow_light"
```

### 保存舵机校准

| 参数 | 必填 | 说明 |
|---|---|---|
| `dir` | 是 | `save_angles` |
| `angles` | 是 | URL 编码后的 JSON，包含 `lf`、`rf`、`lb`、`rb` 四个角度 |
| `action_type` | 否 | `stand`（默认）/ `sleep` / `sit`：要保存的姿态。填写后会立即执行该姿态 |

```js
const angles = encodeURIComponent(JSON.stringify({ lf: 90, rf: 90, lb: 90, rb: 90 }));
fetch(`http://<设备IP>/move?dir=save_angles&angles=${angles}&action_type=stand`);
```

## 表情：`GET /emotion`

| 参数 | 说明 |
|---|---|
| `emotion` | 内置表情名，或 `random`（随机表情） |

内置表情：`neutral` `happy` `laughing` `funny` `sad` `angry` `crying` `loving` `embarrassed` `surprised` `shocked`
`thinking` `winking` `cool` `relaxed` `delicious` `kissy` `confident` `sleepy` `silly` `confused`。未知表情返回 503。

## SD 卡表情文件：`/canvas`

画板上传已停用：`POST` 返回 `410 Gone`。其余功能针对 `/sdcard/custom_emo/` 中的文件，需要已插入 SD 卡。

| 请求 | 说明 |
|---|---|
| `GET /canvas?list=1` | 返回文件名 JSON 数组 |
| `GET /canvas?load=1&name=<文件名>` | 加载并显示该表情 |
| `GET /canvas?delete=1&name=<文件名>` 或 `DELETE /canvas?name=<文件名>` | 删除文件 |
| `HEAD /canvas` | 仅返回响应头 |

## 自定义表情：`/custom_emo`

自定义表情由 `/sdcard/custom_emo/<name>.gif` 与可选的 `<name>.json` 配置组成。配置字段：`music`（SD 卡上的音乐路径）、
`action`（内置动作名）、`custom_action`（自定义动作名）、`loop`（音乐是否循环，默认 `true`）。`action` 和 `custom_action` 不能同时填写。
SD 卡未挂载时返回 503。

| 请求 | 说明 |
|---|---|
| `GET /custom_emo` 或 `?cmd=list` | 返回 `{"ok":true,"dir":"/custom_emo","items":[{name,file,size,music,action,custom_action,loop}]}` |
| `?cmd=get_config&name=<名称>` | 返回配置，以及 `kind`（`replace` 表示内置名称，`custom` 表示自定义）和 `exists`（GIF 是否存在） |
| `?cmd=set_config&name=<名称>&music=&action=&custom_action=&loop=` | 保存配置。内置名称不能保存配置 |
| `?cmd=play&name=<名称>` | 显示该表情，并按配置播放音乐、执行动作 |
| `?cmd=delete&name=<名称>` | 删除 GIF 与配置 |
| `?cmd=reload[&name=<名称>]` | 重新加载；带名称时重新显示该表情 |

名称最长 47 字节，并遵循上文的文件名规则。

## 自定义动作：`/custom_action`

动作文件位于 `/sdcard/custom_action/`。

| 请求 | 说明 |
|---|---|
| `GET /custom_action` | 返回动作名 JSON 数组（未插 SD 卡时返回 `[]`） |
| `GET /custom_action?name=<动作名>` | 返回该动作的 JSON |
| `GET /custom_action?action=<动作名>` | 执行该动作 |
| `POST /custom_action?name=<动作名>` | 以请求体（JSON，1–8192 字节）保存动作 |
| `DELETE /custom_action?name=<动作名>` | 删除动作 |

动作 JSON 示例：

```json
{ "steps": [ { "lf": 90, "rf": 90, "lb": 90, "rb": 90, "delay": 500 } ] }
```

## Wi-Fi 列表：`/wifi_settings`

| 请求 | 说明 |
|---|---|
| `GET /wifi_settings` | 返回 `[{"ssid":"…","password":"…"}]`，顺序即连接优先级 |
| `POST /wifi_settings?ssid=<名称>&password=<密码>` | 添加或更新（SSID ≤ 32 字节，密码 ≤ 64 字节）。持久化失败返回 500 |
| `DELETE /wifi_settings?index=<序号>` | 按 GET 返回的数组下标删除 |

> 该接口会以明文返回已保存的 Wi-Fi 密码，请只在可信的局域网内使用。

## 基本设置：`POST /settings`

请求体为 `application/x-www-form-urlencoded`，长度 1–4096 字节。未提交的字段取默认值。

| 字段 | 取值 | 默认 |
|---|---|---|
| `volume` | 0–100 的十进制整数 | 50 |
| `music_mode` | `single` / `random` / `sequence` | `random` |
| `music_posture` | 播放音乐时的姿态：`stand` / `sleep` / `sit` | `stand` |
| `boot_animation` | 开机灯效 | `true` |
| `charging_led` | 充电灯效 | `true` |
| `free_explore` | 自由探索 | `false` |
| `lang_en` | 英文界面标记 | `false` |

布尔字段接受 `1/true/on/yes` 与 `0/false/off/no`（不区分大小写）。设置保存在 NVS 命名空间 `display` 中，并立即生效。

```sh
curl -X POST "http://<设备IP>/settings" -d "volume=60&music_mode=sequence&free_explore=true"
```

## 本地固件升级：`/web_ota`

- `GET /web_ota`：升级页面。
- `POST /web_ota/upload`：请求体为应用镜像原始字节（如 `build/xiaozhi.bin`），不接受 multipart 或整片合并镜像。
  设备会写入非当前运行的 OTA 分区，校验通过后切换启动分区，返回 `{"ok":true,"rebooting":true,"version":"…"}`，约 1 秒后重启。
  任何失败都会保留当前启动分区。镜像过大返回 413，正在升级返回 409。

```sh
curl -X POST "http://<设备IP>/web_ota/upload" -H "Content-Type: application/octet-stream" --data-binary @build/xiaozhi.bin
```

## SD 卡文件：`/sdcard`

路径通过查询参数 `path` 或请求头 `X-Sd-Path`（URL 编码）传入，以 `/` 或 `/sdcard/` 开头，不允许 `..`。SD 卡未挂载时返回 503。

| 请求 | 说明 |
|---|---|
| `GET /sdcard?path=/` | 列目录：`{"path","total","free","entries":[{name,type,size,mtime}]}` |
| `GET` 或 `HEAD /sdcard/download?path=<文件>` | 下载文件，响应头 `X-File-Size` 为文件大小 |
| `POST /sdcard/upload?path=<文件>` | 以请求体上传；先写临时文件，接收完整后再替换 |
| `POST /sdcard/mkdir?path=<目录>` | 创建目录 |
| `DELETE /sdcard?path=<文件或空目录>` | 删除文件或空目录（不能删除根目录） |
| `/sdcard/format` | 不支持格式化，返回 501 |

## 首页：`GET /`

返回简单的说明页，包含本地固件升级入口。
