# Aster Firmware

Aster 提供 Launcher、首次联网设置、应用安装和 WAMR AOT 运行时。当前 ABI 为 4.5.0。设备固件按开发板构建，应用按 CPU 架构构建。

## 选择开发板

- **立创 ESP32-S3 实战派**。Zephyr，240×320，应用目标 `xtensa`，设备标识 `aster.esp32s3.lckfb`。使用下方默认配置。
- **鱼鹰光电 P4C5**。ESP-IDF 6.0，ST7102 480×480，外接 C5 提供 Wi-Fi。应用目标 `riscv`，设备标识 `osptek-p4-st7102`。[接线、构建与烧录](boards/osptek_p4/README.md)
- **FAKE POD NANO PRO**。ESP32-S31，ESP-IDF 6.1，460×460，应用目标 `riscv`，设备标识 `fakepod-s31`。板上可使用本地播放器、经典蓝牙音频和 USB 读卡器。[板级说明](boards/fakepod_s31/README.md)
- **微雪 ESP32-S3-Touch-LCD-3.5B**。Zephyr 实验性配置，应用目标 `xtensa`。使用 `boards/waveshare_35b/selection.conf` 单独构建。[板级说明](boards/waveshare_35b/README.md)

## 构建与烧录

Zephyr 使用 Python 3.12、west 和 Zephyr SDK。新工作区先按 `west.yml` 初始化依赖并应用驱动补丁。以下命令在固件仓库根目录运行。[工作区补丁](patches/README.md)

```bash
python tools/firmware.py menuconfig
python tools/firmware.py build
python tools/firmware.py flash
```

两块 ESP-IDF 板各自使用对应版本的 IDF 环境和独立构建目录，命令见上方板级说明。已发布的预编译镜像及适用板型见 [固件 Release](https://github.com/hello-myj/aster-firmware/releases)。

## 首次开机与应用安装

在已发布初始化资源的板上插好 SD 卡，首次开机按屏幕提示用手机连接设备热点，在网页填写 Wi-Fi。设备从默认 OBS 源下载并校验系统字体和本机商城，完成后进入桌面。已有完整资源的卡可离线启动。自建服务或更换证书见 [首次开机](docs/first-boot-setup.md) 和 [下载源配置](docs/distribution-config.md)。

开发者可从 SDK 通过串口安装 AOT 包。以下以 RISC-V 为例，立创板使用 `xtensa`。固件在安装时核对架构、ABI 和包完整性，在启动时核对清单中的 `supported_boards` 和 `requires`。

```text
aster deploy dist/riscv/<应用>.ast --target riscv --port <串口> --run
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
common/                   跨系统的接口、媒体、网络及安装包解析
systems/zephyr/           Zephyr 服务、运行时和共享桌面实现
systems/espidf/           ESP-IDF 服务、运行时与板级组件
```

系统字体：

```bash
python tools/resources.py
```

首次开机引导负责准备 SD 字库。自行重建字体后需重新分发资源；烧录镜像不会改写卡上的字形。字体与光带的像素对齐、资源重建及验证说明见 [桌面显示清晰度](docs/ui-display-clarity.md)。

## 检查

```bash
python tools/font_metrics.py --check
python tools/export_contract.py
python tools/host.py
python -m unittest discover -s tests -v
```

架构见 [docs/architecture.md](docs/architecture.md)。从工程到设备的步骤见 [快速入门](docs/quickstart.md)。许可见 [NOTICE.md](NOTICE.md)。

## 新板适配

同架构、同 AOT 配置的新板可以沿用现有 SDK。固件通过板型 ID 和运行时能力描述设备。ESP-IDF 的音频编解码器、外接 C5 和板上接线由各板注册。实现入口及可选服务见 [平台适配接口](docs/platform-adapter-api.md)。
