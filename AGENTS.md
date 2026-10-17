# AGENTS.md

nomifun-esp32-dog：基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) v2.0.5 的 ESP32-S3 桌面机器狗固件，板型 `zfsn-esp32s3-dog`，使用 ESP-IDF 5.x（已验证 v5.5.5）构建。

## 目录

- `main/boards/zfsn-esp32s3-dog/`：板级实现（舵机动作、OLED 菜单、按键、灯带、电源、SD 媒体与闹钟、Web 控制、MCP 工具）。
- `components/78__esp-wifi-connect`、`components/78__esp-ml307`：带本地修改的组件副本，由 `main/idf_component.yml` 的 `override_path` 引用。
- `partitions/v2/zfsn_16m.csv`：16 MB 分区表。
- `tests/dog_host/`：Windows 主机测试；`tools/`：设备备份、刷写与串口采集工具。
- `docs/dog/`：硬件、HTTP API、MCP 工具、菜单、构建与烧录文档；`hardware/`：原理图与 PCB 工程。

## 规则

- 板级行为放在板级目录；核心代码只依赖 `Board` 接口，必要的核心改动用 `CONFIG_BOARD_TYPE_ZFSN_ESP32S3_DOG` 守卫。
- 不改分区布局：nvs 0x9000/0x4000、otadata 0xd000、phy_init 0xf000、ota_0 0x20000/0x600000、ota_1 0x620000/0x600000、assets 0xc20000/0x3e0000。
- 板型标识 `zfsn-esp32s3-dog`、`PROJECT_VER`（2.1.0）和 CMake 工程名 `xiaozhi`（作为应用名上报）参与 OTA 识别，不要随意修改。
- NVS 键是持久接口，变更需给出迁移方案。
- `main/boards/zfsn-esp32s3-dog/assets/dog_assets.bin` 是预构建资源镜像，不要手工修改。
- 不手工编辑生成物：`build/`、`managed_components/`、`sdkconfig`、`main/assets/lang_config.h`。
- 设备备份与串口日志可能包含 Wi-Fi 密码等隐私，只能放在 `.private/`（已忽略），不得提交。
- 汇报时区分验证等级：已实现 / 本地测试与构建通过 / 真机验证 / 声学·机械·实屏验收 / 长期稳定性。构建通过不等于真机验证。

## 命令

- 构建：`idf.py set-target esp32s3`（首次），然后 `idf.py build`。`sdkconfig.defaults.esp32s3` 已默认选中本板型。
- 主机测试与工具用法见 `tests/dog_host/README.md`、`tools/README.md`。
