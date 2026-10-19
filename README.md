# Aster Firmware

立创 ESP32-S3 实战派（`lckfb-s3`）和微雪 ESP32-S3-Touch-LCD-3.5B（`waveshare-35b`）的设备固件。当前 ABI 为 4.0.0。桌面固定竖屏；APP 可以声明横屏或竖屏。

## 构建和烧录

需要 Python 3.12、west 和 Zephyr SDK。`west.yml` 锁定 Zephyr v4.4.2。

```bash
python tools/firmware.py menuconfig
python tools/firmware.py build
python tools/firmware.py flash
```

固件命令没有 `--target`。板型和功能保存在 `config/aster.conf`，首次使用时从 `config/default.conf` 加载。产物在 `build/firmware/`。

微雪板使用独立配置和构建目录：

```bash
python tools/firmware.py build --config boards/waveshare_35b/selection.conf --build-dir build/waveshare-35b
python tools/firmware.py flash --config boards/waveshare_35b/selection.conf --build-dir build/waveshare-35b
```

接线、引脚和 USB 调试配置见 [boards/waveshare_35b/README.md](boards/waveshare_35b/README.md)。新工作区用 `west init -l /path/to/aster-firmware` 和 `west update` 初始化，驱动补丁见 [patches/README.md](patches/README.md)。

## 安装应用

设备先刷入本固件，并把 `dist/sdcard/` 复制到 SD 卡根目录。应用包由 SDK 用 `--target xtensa` 构建，ABI 最低为 4.0。3.x 包在安装和启动时被拒绝，需要用当前 SDK 重新编译。

```bash
aster deploy dist/xtensa/<应用>.ast --target xtensa --port <串口> --run
```

## 显示和 Wi-Fi

设备 shell：

```text
aster display headless
aster display portrait
aster sys info
```

有 APP 运行时，手动切换显示会返回忙，先停止该 APP。APP 在 `app.toml` 的 `[app]` 下设置 `orientation = "landscape"` 或 `"portrait"`。

下拉快捷设置进入 Wi-Fi 或个人热点。热点地址为 `192.168.4.1/24`，DHCP 从 `192.168.4.10` 起，不提供 NAT。

## 网络

默认随 Wi-Fi 启用。APP 使用六个导入：`aster_net_info`、`aster_net_open`、`aster_net_io`、`aster_net_status`、`aster_net_cancel`、`aster_net_close`。设备 shell 仍用 `aster net` 做诊断。

- [TCP/UDP](docs/network-transport-cli.zh-CN.md)
- [HTTP/HTTPS](docs/network-http-cli.zh-CN.md)
- [MQTT/MQTTS](docs/network-mqtt-cli.zh-CN.md)
- [WebSocket/WSS](docs/network-websocket-cli.zh-CN.md)

## 目录

```text
config/                   配置
hardware/                 板卡描述
boards/                   设备树和板级配置
contract/include/aster/   公开 ABI
systems/zephyr/           设备服务和桌面
```

系统字体：

```bash
python tools/resources.py
```

## 检查

```bash
python tools/font_metrics.py --check
python tools/export_contract.py
python tools/host.py
python -m unittest discover -s tests -v
```

架构见 [docs/architecture.md](docs/architecture.md)。从工程到设备的步骤见 [快速入门](docs/quickstart.md)。许可见 [NOTICE.md](NOTICE.md)。
