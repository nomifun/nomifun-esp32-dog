# tools 设备工具

针对 ESP32-S3 dog（16 MB Flash、固定六分区布局）的备份、刷写与诊断脚本。
所有工具共享 `device_workflow.py` 的安全约束：只允许白名单内的 esptool
子命令（`chip_id`、`flash_id`、`get_security_info`、`read_flash`、
`verify_flash`、`write_flash`），拒绝 `--force`/擦除/加密参数；需要串口的
命令一律要求显式 `--port` 并校验端口为已枚举的 USB 串口设备；写入前校验
芯片型号、Flash 容量、分区布局与镜像头，写入后逐项回读校验。

备份、串口日志、阶段目录与回执默认写入 `ROOT/.private/`（如
`.private/backups/<时间戳>-backup-<mac>/`），Windows 下目录 ACL 被收紧为仅
当前用户与 SYSTEM 可读。**`.private/` 内的数据可能包含 Wi-Fi 凭据等私密
信息，严禁发布。**

## device_workflow.py

整机备份与受控刷写入口。

- `list`：仅枚举串口元数据，不打开端口。
- `prepare [--build <dir>]`：纯离线校验 build 目录（`flasher_args.json`、
  `config/sdkconfig.json`、`project_description.json`、五个写入段、分区表
  MD5、镜像头/校验和/SHA-256），默认 `--build build/`。
- `check-backup --manifest <path>`：纯离线校验整机备份清单与哈希。
- `probe --port <p>`：读取芯片/Flash/安全信息并保存 `probe.json`。
- `backup --port <p>`：整片 16 MB 读回，生成 `flash-16mb.bin`、单独导出的
  `nvs.bin`/`phy_init.bin` 与 `manifest.json`，随后 verify 并标记
  `status: verified`。
- `flash --port <p> --manifest <path> [--build <dir>]`：先验证备份清单，
  再用 verify 确认**整片 Flash 与备份完全一致**（若 NVS 被正常使用改变，
  必须先重新备份），才执行五段写入；写后回读校验全部写入段与被保留的
  `nvs`/`phy_init`，然后 `hard_reset` 并写回执。不触及 `ota_1`。
- `monitor --port <p> [--seconds N]`：被动抓串口日志（不主动复位），
  输出 `serial.log`。

## stage_app_update.py

离线生成 app-only 更新阶段目录：取新 build 的 `app.bin` 与配置元数据，配
合既有部署包中其余四个不可变段，产出一个只能写 `app` 的“伪 build”目录及
`manifest.json`。不打开串口、不访问网络；新 build 的 bootloader 会被校验
但绝不入选写入。

## update_app.py

单段 app 更新（唯一写入 `0x20000`）。前置校验：初始整机备份、上一次已验
证回执（支持整机回执或 `app-only`/`assets-only` 单段回执）、不可变部署
包、当前 otadata 只接受初始 OTA0 或首次启动后的 OTA0 状态、新 build 的其
余四段必须与已部署一致、app 变化需显式（`--reflash-unchanged` 允许有意重
写相同镜像）。写入前先用设备端 MD5 校验旧段与 OTA1 未变，并快照
`nvs`/`phy_init`/`otadata`；写后逐项 verify，最后校验被保留区域并
`hard_reset`，写 `receipt.json`。

## write_assets.py

把 `main/boards/zfsn-esp32s3-dog/assets/dog_assets.bin`（固定大小与
SHA-256）写回 `0xc20000` assets 分区。

- `stage --manifest <m> --previous-receipt <r> --previous-package <p>
  [--output <dir>]`：离线生成 assets-only 阶段目录，`assets.bin` 替换为
  `dog_assets.bin`，其余四段来自部署包；要求已部署 assets 与该镜像不同。
- `write --manifest <m> --previous-receipt <r> --previous-package <p>
  --build <stage> --port <p>`：同样的设备侧前置校验（MAC/Flash/安全状态、
  备份一致、otadata、被保留区域快照），唯一写 `0xc20000`，写后回读并验证
  其余区域未被改动。

## capture_boot.py

受限串口捕获：`--port` 必须为 ESP32 USB JTAG（VID=303A PID=1001），时长
1–300 秒，数据量上限 4 MB。默认先 `HardReset` 引导应用（`--no-reset` 为纯
被动监听）；USB 重新枚举时只跟随同一设备身份。原始 `serial.log`、
`diagnostics.json`（错误行）与打点的 `summary.json`（版本/状态/告警计数，
不回显任意串口行）写入受限目录。

## capture_panel_frames.py

离线解码串口日志中的 `DOG_PANEL_V1` 帧记录：校验 id 递增、seq/after 严格
递增、16 个有序分片与 IEEE CRC-32，再把 SH1106 页格式转换为 128×64 灰度
PNG（原生 + 放大）并写 `manifest.json`。从不回显日志中的任意行；配合
`capture_boot.py` 的日志使用（固件需启用面板捕获记录）。
