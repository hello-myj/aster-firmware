# Aster Firmware

独立固件仓库，不需要 SDK 或原 MJX 源码。当前支持 **立创开发板 ESP32-S3 实战派（N16R8）**，并新增实验性 **微雪 ESP32-S3-Touch-LCD-3.5B** 板级方案，launcher 固定竖屏；同一固件保留 APP 横屏、竖屏和无屏操作能力。

## 配置、构建、烧录

需要 Python 3.12、west、Zephyr SDK，以及 `west.yml` 锁定的 Zephyr v4.4.2。当前工作区激活已有 `.venv` 后，在本仓库执行：

```bash
python tools/firmware.py menuconfig
python tools/firmware.py config
python tools/firmware.py build
python tools/firmware.py flash
```

**固件没有 `--target` 参数。** menuconfig 在 Zephyr 生成设备树之前运行，选择值保存在 `config/aster.conf`：

- System and hardware：系统和真实硬件型号；可选 Zephyr / 立创 ESP32-S3 实战派或微雪 3.5B。
- Startup display：默认竖屏，可选无屏；APP 的横竖屏由应用声明决定。
- Firmware services：相机、音频、Wi-Fi、原生桌面等功能。

构建工具从上述配置推导底层 Zephyr board、设备树和功能选项。产物统一在 `build/firmware/`。`config/aster.conf` 不纳入 Git，首次使用加载 `config/default.conf`；清理构建目录不会删除配置。高级 Zephyr 选项可在已配置的构建目录使用 `west build -d build/firmware -t menuconfig` 查看；系统/硬件选择以项目入口保存的配置为准。

微雪板使用项目内独立 Zephyr board；接线、外设范围和验证说明见 [boards/waveshare_35b/README.md](boards/waveshare_35b/README.md)。可保留原配置并独立构建：

```bash
python tools/firmware.py build --config boards/waveshare_35b/selection.conf --build-dir build/waveshare-35b
```

`--config` 与 `--build-dir` 同样适用于配置查看和烧录；烧录前工具检查目标是否与镜像一致。
微雪板仅连接 Type-C 时，使用 `boards/waveshare_35b/usb-debug.conf` 和独立 `build/waveshare-35b-usb` 构建目录可开启 USB shell；该模式暂停 USB 文件服务，详见板级说明。

新的 Zephyr 工作区可通过 `west init -l /path/to/aster-firmware`、`west update` 初始化，再安装该版本环境依赖。参考板需要 [patches/README.md](patches/README.md) 中的驱动补丁。当前工作区已存在相应补丁，本次没有修改共享 Zephyr 源码。

## 同一固件切换显示模式

设备 shell：

```text
aster display headless
aster display portrait
aster sys info
```

切换请求在拥有 UI 的工作线程执行。无屏模式关闭显示输出和触摸能力，对应的能力查询返回不可用；串口、存储、网络及非图形 APP 继续可用。恢复显示后返回竖屏 launcher。桌面没有横屏布局或方向切换入口；原生 launcher 启用时，手动请求横屏返回不支持。

有 APP 正在运行时，手动模式切换返回忙，需要先停止该 APP；APP 的 `app.toml` 中 `[app]` 下仍可设置 `orientation = "landscape"` 或 `orientation = "portrait"`。启动前按声明切换方向；正常退出、停止、异常及启动失败均在返回 launcher 前恢复竖屏。未声明方向的 APP 从竖屏启动。已经安装的 APP 在启动时也检查当前能力，避免无屏模式下启动强依赖显示的应用。切换命令不修改默认启动配置，也不改变硬件型号。

## Wi-Fi 与个人热点

在主页或应用内下拉快捷设置，点击 Wi-Fi 行的“设置”。两个功能使用独立页面：

- Wi-Fi：扫描列表在 240×320 屏幕上一屏显示 5 个完整网络，支持滚动。点击网络输入密码，也可手动添加隐藏网络；连接卡片显示名称和 IP，详情页提供断开、重新连接和忘记网络；忘记操作需二次确认。
- 个人热点：设备自身提供 WPA2 热点，可设置名称、8–63 位密码并开启或关闭。修改时密码留空保留原密码；键盘支持大小写、数字和 ASCII 符号。

连接、扫描和热点操作由独立 Wi-Fi 工作队列执行。成功的网络与热点配置分别保存在 SD 卡系统 KV；启动时自动重连 Wi-Fi，个人热点保持关闭，手动打开时使用保存的配置。

个人热点本机地址为 `192.168.4.1/24`，DHCP 地址池从 `192.168.4.10` 开始。支持 Wi-Fi 与热点同时开启；热点提供局域网连接，目前没有 NAT 或互联网共享。

## 目录与资源

```text
config/                   menuconfig 定义、默认配置、本机配置
hardware/                 真实板卡信息和固件私有构建映射
boards/                   接线、设备树和功能配置片段
contract/include/aster/   公开 ABI、版本和字体度量
platform/                 无 OS 依赖的 HAL 声明、硬件端口与驱动
systems/zephyr/           完整服务、WAMR 集成、桌面、shell 和启动
systems/posix/            开发用核心 ABI 测试程序，不是另一款固件
modules/                  WAMR、相机、图形和音频组件
third_party/              LVGL 与配置 UI 的独立副本
resources/fonts/          系统字体及生成记录
```

系统字体归固件所有：

```bash
python tools/resources.py
```

把 `dist/sdcard/` 的内容复制到设备 SD 卡根目录，得到 `aster/fonts/ui_<px>.cpfont` 与 `faces.json`，覆盖 12/14/16/18/20/24/28/32/40/48px 原生字号。重烘焙字体可安装 `fonttools`、`freetype-py` 后使用 `tools/build_cpfont.py`，再运行 `tools/font_metrics.py` 更新公开度量。APP SDK 不负责生成或部署系统资源。

## 契约发布与开发测试

```bash
python tools/font_metrics.py --check
python tools/export_contract.py
python tools/host.py
python -m unittest discover -s tests -v
build/host/bin/aster-host tests/fixtures/hello.wasm
```

契约 ZIP 包含公开接口、导入符号、WAMR 版本、真实硬件描述及 SHA-256 清单，不包含引脚、DTS 或固件构建规则。SDK 显式导入发布物，日常构建不需要固件仓库。

`aster-host` 仅为 Linux x86_64 上的核心 ABI / APP 生命周期测试程序，不注册为硬件型号，不提供完整屏幕、存储、音频或摄像头模拟。它不需要 Zephyr 或 SDK；开发测试最多处理 600 帧，不限制单个无法返回的 WASM 调用。

架构与移植边界见 [docs/architecture.md](docs/architecture.md)，P0 实测结果与剩余硬件验收见 [可靠性验收记录](docs/reliability-validation.md)，许可见 [NOTICE.md](NOTICE.md)。实机显示、音频、触摸等仍需开发板验证。

APP 从独立工程到设备的步骤见 [快速入门](docs/quickstart.md)，ABI 报告、固定旧包与三仓库候选发布检查见 [发布验证](docs/release-validation.md)。

固定图形场景的真机数据、合批收益及测量边界见 [图形基准](docs/graphics-benchmark.md)。模拟器确定性回放与故障诊断说明在 aster-sim/docs/replay.md。

构建工具会拒绝构建目录根部未显式选择的 `.conf` 文件：Zephyr 会把这些文件自动追加到配置末尾，可能覆盖板型或遗留诊断开关。请将独立配置放在构建目录之外并通过 `--config` 指定。镜像校验同时核对 Kconfig 板型与设备树目标，避免同 CPU 的不同开发板配置混用。
