# 构建与烧录

## 环境

- ESP-IDF 5.4 及以上的 5.x 版本，已在 v5.5.5 上验证。暂未适配 ESP-IDF 6.x。
- Windows 可使用 ESP-IDF 安装器；Linux / macOS 先执行 `install.sh`，再执行 `. ./export.sh`。
- 设备通过 USB-Serial-JTAG 连接电脑，日志也从这个口输出。

## 构建

```sh
idf.py set-target esp32s3   # 只需执行一次
idf.py build
```

`sdkconfig.defaults.esp32s3` 已经选好了板型、DIO Flash、USB-Serial-JTAG 控制台和 FATFS 长文件名，不需要再进 menuconfig。

主要产物：

| 文件 | 写入位置 |
|---|---|
| `build/bootloader/bootloader.bin` | 0x0 |
| `build/partition_table/partition-table.bin` | 0x8000 |
| `build/ota_data_initial.bin` | 0xd000（otadata） |
| `build/xiaozhi.bin` | 0x20000（ota_0） |
| `main/boards/zfsn-esp32s3-dog/assets/dog_assets.bin` | 0xc20000（assets，预构建镜像，构建时不会重新生成） |

## 烧录

### 1. 先备份整片 Flash

首次刷写前强烈建议先备份，以便回到刷写前的固件：

```sh
esptool.py --chip esp32s3 -p <串口> -b 460800 read_flash 0 0x1000000 backup-16mb.bin
```

备份文件里有 NVS 中保存的 Wi-Fi 密码、设备身份等隐私数据，请妥善保管，不要上传或公开。

### 2. 刷写固件

```sh
idf.py -p <串口> flash monitor
```

`flash` 会写入上表中的全部 5 个部分，并把 otadata 初始化为从 ota_0 启动。NVS 不会被擦除，已保存的 Wi-Fi、舵机校准和设置都会保留。
如果无法自动进入下载模式，可以按住 BOOT 键再插入 USB 线。

### 只更新应用

```sh
idf.py -p <串口> app-flash
```

`app-flash` 只写入 ota_0（0x20000）。如果设备此前通过 OTA 切换到了 ota_1，还需要执行 `idf.py -p <串口> erase-otadata`，
让设备重新从 ota_0 启动。

设备联网后，也可以打开 `http://<设备IP>/web_ota` 上传 `build/xiaozhi.bin` 进行本地升级，详见 [http-api.md](http-api.md#本地固件升级web_ota)。

### 恢复备份

```sh
esptool.py --chip esp32s3 -p <串口> -b 460800 write_flash 0 backup-16mb.bin
```

### 带校验的工具

[`tools/`](../../tools/) 提供带检查的备份、刷写、只更新应用、写入资源分区和串口采集脚本。它们会先核对芯片、Flash 与分区布局，
再写入并回读校验，备份和日志保存在被 Git 忽略的 `.private/` 目录。用法见 [tools/README.md](../../tools/README.md)。

## 配网与服务器

- 首次启动或清除网络设置后，设备会开启热点 `Xiaozhi-XXXX`。连接后打开 `http://192.168.4.1`，在同一页面填写 Wi-Fi 和 OTA 地址。
- OTA 地址默认是 `https://api.tenclass.net/xiaozhi/ota/`，也可以在 menuconfig 的 `OTA_URL` 中修改默认值。
- 启动时 OTA 检查连续失败会进入错误提示，并显示 BOOT 恢复入口：长按 BOOT 5 秒后松开，即可清除连接设置并重新配网。
- `PROJECT_VER` 为 2.1.0，OTA 服务端会据此判断是否需要升级。

## 诊断构建：OLED 帧采集

```sh
idf.py -DZFSN_UI_CAPTURE_BUILD=ON build
```

这个构建会在启动后依次预览各个菜单页面（不会执行确认操作），并把实际提交给 SH1106 的每一帧输出到串口日志。
用 `tools/capture_boot.py` 采集日志后，再用 `tools/capture_panel_frames.py` 解码成图片。
用完后需要执行 `idf.py -DZFSN_UI_CAPTURE_BUILD=OFF build` 恢复正常构建。

## 主机测试

不依赖设备的 Windows 主机测试见 [tests/dog_host/README.md](../../tests/dog_host/README.md)。
