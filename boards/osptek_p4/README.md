# OSPTEK ESP32-P4 + C5 / ST7102 480×480

状态：实验性接入；2026-10-31 已完成首轮 P4 v3.2 实机启动检查，未完成整板验收。
固件分支 `feat/osptek-p4-st7102-idf`，配套 SDK 分支 `feat/osptek-p4-aot`。
此描述符位于 `boards/`，不把它发布为已验证的 `hardware/` 设备。

采用 ESP-IDF 6.0：现有 ST7102/ST7123 例程、P4 MIPI-DSI 和 C5 ESP-Hosted
驱动均可直接复用。Zephyr 保留原 S3 目标；共享的应用、文件、图形和音频逻辑
通过独立的 IDF 组件接入，未把原 S3 平台切换到 IDF。

## WSL 构建

需要 ESP-IDF 6.0、ESP32-P4 工具链和对应 Python 环境。

```bash
source ~/esp/esp-idf/export.sh
cd /mnt/g/proj/aster/aster-firmware
python tools/bringup.py osptek-p4 build
# 旧版硅片使用隔离目录；需先核实手上的 P4 修订版本：
python tools/bringup.py osptek-p4 build --revision pre3
python tools/bringup.py osptek-p4 menuconfig
```

默认是 P4 v3.x，最低修订版本 3.0。产物在 `build/osptek-p4/`。
烧录须使用完整的 bootloader、分区表和应用参数，不能把应用直接写到 0：

```bash
python tools/bringup.py osptek-p4 flash --port /dev/ttyACM0
python tools/bringup.py osptek-p4 monitor --port /dev/ttyACM0
```

2026-10-31 已通过 Windows CH343 串口烧录 P4，未改写 C5 固件。
当前设备需使用 esptool 的 `--no-stub` ROM 下载模式；默认 stub 启动未成功。

## 首轮实机检查（2026-10-31）

识别为 ESP32-P4 v3.2、16 MB Flash、32 MB PSRAM（200 MHz，内存测试通过）。
修复厂商旧例程的 MIPI PHY 默认时钟：IDF 6 中旧名称指向 pre3 时钟，
rev3 会触发 abort；现由 IDF 按芯片修订选择时钟源。

实机日志确认：ST7102 读 ID 成功，RGB565 显示初始化完成；ST7123 地址 0x55
响应并报告 480×480；ES8311 检出 ID 0x83；C5 SDIO 链路建立，扫描得到 13 个 AP；
FLUX 进入桌面且串口命令可响应。用户确认 2048 的文字/棋盘显示正常，滑动可移动
方块。音频输入输出的实际声音、联网与 TLS 尚未完成验收。

首轮未插卡时 SD 挂载 `ESP_ERR_TIMEOUT`。插入 SD 卡并重新上电后已正常挂载：
SDHC、14910 MB、4-bit/20 MHz，C5 同时正常建立 slot 1 链路。通过读卡器写入
20 个字体、应用和诊断文件，共 96607338 字节，逐个 SHA-256 校验通过。
字体加载正常；已安装 Hello、2048、Gfx Show、Continuous Audio 和 Network Echo。

RISC-V AOT 已在 P4 v3.2 执行：600 帧显示诊断达到帧数上限后返回桌面
（诊断约定结果 124）；Hello 完成生命周期并退出 0；2048 绘制 480×480 首帧，
暂停/恢复状态为 2/1，停止后退出 0，随后再次通过 AOT 启动并恢复历史分数。
没有在这些运行中回退解释器。后 3 个应用目前仅验证安装。

实机修复：IDF 分配器拒绝 `EXEC|8BIT` 组合，P4 可执行 SRAM 改为请求
`INTERNAL|EXEC`；WAMR 的 PSRAM 分配与重分配保证 8 字节对齐；控制台安装
调用链超过默认 4 KiB 栈，将 REPL 栈改为 16 KiB。相应运行时主机回归通过。

桌面运行后 `mem-dump`：内部空闲 323847 字节（约 316 KiB），最大连续块
241664 字节（236 KiB），历史最低空闲 316415 字节（约 309 KiB）；PSRAM 空闲
31762076 字节（约 30.3 MiB）。这是未加载字体/应用时的数据。
加载字体并运行 2048 AOT 后，内部空闲 261923 字节（约 256 KiB），最大连续块
188416 字节（184 KiB），历史最低 252523 字节（约 247 KiB）；PSRAM 空闲
30653460 字节（约 29.2 MiB）。这不是所有功能同时开启后的峰值。

本机证据保存在忽略目录 `build/osptek-p4/hardware/`：`second-boot.log`、
`runtime-probe.log`、`wifi-results.log`、`sd-inserted-boot.log`、`aot-second-run.log`、
`installed-aot-run.log`、`aot-lifecycle.log`、`final-apps.log`。
烧录前备份了本次覆盖范围的前 2 MiB，
文件 `original-first-2MiB-20261031.bin`，SHA-256：
`3afa97679b861eadad77b9202dcd9f8d0978455efac0b45b6fb67fea2345c3b0`。

## 已接入的软件

- FLUX 桌面、字体、触摸手势、快捷设置、返回桌面及原生 Wi-Fi 设置页。
- ST7102 RGB565 显示与 ST7123 五点触摸；保留控制器固定槽位，错误读数不合成为抬手。
- RGB565 surface、脏矩形、居中等比缩放和反向触摸映射；原图形引擎、字体、BMP/JPEG/PNG。
- WASM 和 RISC-V AOT，应用生命周期、暂停/恢复、停止、回调超时终止与资源清理。
- SD 文件、目录、应用沙箱、权限、KV；ASTP 安装、扫描、卸载与双版本代码目录。
- C5 SDIO Wi-Fi、扫描/连接/保存配置/重连/忘记、受密码保护的本地 AP；SNTP 时钟。
- 原有 TCP/UDP、HTTP/HTTPS、MQTT/MQTTS、WebSocket/WSS 服务和六个网络 ABI 导入。
- ES8311/I2S 播放和录音；复用原 WAV/PCM/MP3、歌词、音效混音及压缩流逻辑。

C5 主机固定使用 ESP-Hosted 2.12.9，与板包提供的协处理器版本相同。
SD 卡占 slot 0、C5 占 slot 1，共享控制器初始化并按槽释放。
P4 的 SAR ADC 在这个配置中用于本地 RNG 熵源；外部 ES8311 录音不使用它。

## P4 Launcher 性能

同一块 P4 v3.2 / 480×480 板，5 个已安装应用、共 9 个桌面条目。使用
`aster ui bench 8`：预热 2 秒，然后每 180 ms 向下一张卡片滚动，测量 8 秒。
这是实际提交到显示缓冲的 UI 帧率，不是 MIPI 扫描刷新率，也不是触摸到显示的延迟。

- 原始 IDF 适配：10.1 FPS，平均绘制 74.0 ms，帧间隔 P95 为 117 ms。
- 修复 RGB565 快速绘制路径、刷新调度及整屏失效：23.6 FPS。
- 加入 PPA 填充、DMA2D 拷贝和整屏容量的局部绘制缓冲：28.3 FPS。
- 加入有界卡片缓存：30.9 FPS，平均绘制 26.9 ms，帧间隔 P95 为 41 ms。

最终固件复测 32.1 FPS、平均绘制 25.7 ms、帧间隔 P95 为 40 ms，PPA 错误数仍为 0。
用户实机确认上下滑动、松手惯性和下拉设置明显更流畅，画面正常。

最后一轮 8 秒内 PPA 完成 325 次填充、29229457 像素、0 次错误；启动时在真实硬件上
校验 RGB565 颜色、部分矩形填充和缓存一致性。只加速大于等于 4096 像素的纯色、
不透明矩形，其他 LVGL 任务继续使用软件路径。PPA 不支持卡片当前的小角度旋转，
不能用其 90° 步进旋转或 1/16 步进缩放替换连续动画。

DMA2D 使用 IDF 6 的 DPI 拷贝接口；提交后等待完成再允许 LVGL/WASM 复用源缓冲，
并使 CPU 的显存缓存失效以保证快照正确。LVGL 使用单个 64 字节对齐、整屏容量的
PSRAM 局部绘制缓冲，避免重复处理 12 个小条带。快照恢复先批量转换字节序，再做
一次整块传输；内存不足时保留逐行恢复路径。

P4 额外保留最多 8 张非活动卡片位图（最多 806400 字节 PSRAM），切换页面、字体
更新或离开 Launcher 时清空；S3 默认不启用这部分缓存。像素测试覆盖原 S3 字节序
与 P4 原生字节序、裁剪、混合和抗锯齿，13 条目循环测试覆盖缓存绕回和退出回收。
137 项固件测试通过（跳过 3 项），S3 编译通过，2048 AOT 的启动/暂停/恢复/停止通过。

正常上电约 3.2 秒进入桌面，不再自动执行 SD 根目录诊断；需要自动诊断时可设置
`CONFIG_ASTER_P4_AUTORUN_DIAGNOSTIC=y`。手动诊断和性能测量：

```text
aster app diagnostic
aster app stop
aster ui bench 8
aster ui perf
mem-dump
```

基线及实机日志位于本机忽略目录 `build/osptek-p4/hardware/` 的 `ui-baseline.log`、
`ui-fast.log`、`ui-hw.log`、`ui-final-bench.log`、`ui-final-repeat.log`、`ui-app-regression.log`。

## AOT

默认 `CONFIG_WAMR_ENABLE_AOT=y`，同时保留快速解释器。
加载已安装应用时优先 `app.aot`；只有在进入 guest 之前加载/实例化失败时才回退
`app.wasm`。运行过 guest 之后的异常不会重试，避免重复副作用。

```bash
# 固件仓库：为单个诊断 WASM 生成 P4 AOT
python tools/p4_aot.py input.wasm output.aot
# 配套 SDK 仓库：生成可安装包
python tools/build.py apps/hello --target riscv --aot
```

目标为 RV32、ILP32F、M/A/F/C，启用软件内存/栈边界检查。
WAMR 2.4.3 只给共享内存模块插入循环终止检查，因此工具对 AOT 编译副本设置
有上限的共享内存，并打开 thread manager。原 portable WASM 不变，不引入 guest
线程。加载器拒绝不满足该配置的 AOT，转入解释器回退流程。

IDF 6 的动态可执行堆与 `CONFIG_ESP_SYSTEM_MEMPROT` 互斥；这个私有配置关闭了该项。
AOT 代码放内部可执行 SRAM，以双核 `fence.i` 同步；较大的镜像可能因 SRAM 不足
回退解释器。AOT 是原生机器码，必须来自受信任的编译/包来源。当前默认未强制包签名；
头部、权限和软件边界检查不能把任意原生 AOT 变成安全沙箱。

## SD 卡与命令

字体资源约 96 MB。生成的桌面目录不含自动启动诊断文件：

```bash
python tools/p4_sdcard.py --package ../aster-sdk/dist/riscv/aster.hello.ast
# 复制 build/osptek-p4/sd-desktop/ 的内容到 FAT SD 卡根目录
```

串口控制台：

```text
aster app list
aster app install /SD:/aster/install/aster.hello.ast
aster app run aster.hello
aster app pause
aster app resume
aster app stop
aster app uninstall aster.hello
aster wifi scan
aster wifi results
aster wifi connect "SSID" "PASSWORD"
aster wifi status
aster wifi reconnect
aster wifi forget
```

Wi-Fi 工作在后台；连接成功后才保存凭据。热点从桌面 Wi-Fi 设置页控制，无路由/NAT。
SD 挂载不会自动格式化。此端口支持 SD 安装和共享 UART 上的 ASTERFS 3 应用包传输；
尚未提供独立 USB-CDC 文件服务和完整的硬件调试命令集。

共享下载串口支持现有 SDK 的 `ASTERPING / ASTERPUT`、512 字节逐块 ACK、CRC32
及 `.part` 断点续传。只接收 SD 根目录或 `/SD:/aster/install/` 下的 `.ast` 安装包，
拒绝路径穿越、直接覆盖已安装应用目录及其他类型文件。完整校验通过后才发布包文件。
UART 正文直接读驱动，保留二进制 CR/LF/NUL；接收期间由控制台线程独占输入。

```text
python tools/deploy.py dist/riscv/aster.touchtest.ast --target riscv --port COM11 --run
```

以上命令在 SDK 仓库运行，端口按实际设备修改。P4 控制台提供标准提示符
`aster:~$`、`installed OK` 和 `started <id>` 应答；`aster app start` 与 `run` 均支持。
已使用原有 SDK 部署工具在实机上传、安装并启动 TouchTest：480×480、RISC-V AOT、
状态 RUNNING。早期单点适配已由用户确认显示与单指轨迹正常。
现已接通五点输入：按控制器原始槽位上报 DOWN/MOVE/UP，同帧事件在绘制前交付，
`aster_input_touch` 返回全部有效触点（兼容原有 12 字节点结构，稳定槽位见事件）。
读取失败或没有新坐标帧时保留原状态；多指期间抑制应用上方的单指系统手势，直到全部抬起。
五指事件、稀疏槽位、独立抬起、复用、坐标越界与异常读取已通过真实 WASM/x86 AOT
桥接回归；新版已烧录实机，TouchTest AOT 运行时报告 `reported_max=5`；
屏幕五指交叉与独立抬起的最终实机效果待用户确认。
应用将最大内存改为 2 MiB，以容纳 480×480 画布及初始内存/托管堆预算。
138 项固件测试通过（跳过 3 项），包含实际接收核心的分片、CRC、断点恢复、
坏包不覆盖旧包、已完整部分文件发布及路径限制测试。部署日志见本机
`build/osptek-p4/hardware/touchtest-serial-final-deploy.log`、`touchtest-running.log`。

`check_runtime.py` 另行生成 `build/osptek-p4/sdcard/app.wasm` 和 `app.aot`。
放到卡根目录后用 `aster app diagnostic` 运行蓝/橙格子诊断，最多 600 帧
（30 fps 约 20 秒），随后返回桌面；自动启动默认关闭。
正常已安装应用没有这项帧数限制。`make_smoke.py` 会覆盖该诊断 WASM。

## 验证范围与已知边界

Python 3.12 / Linux：

```bash
python tools/host.py
ASTER_TEST_RENDER=1 python systems/espidf/tests/check_runtime.py
python systems/espidf/tests/check_ui.py
python -m unittest discover -s tests
```

主机测试执行真实 WASM 与 x86 AOT，覆盖内存边界、像素、触摸、缩放、定时器、
文件/KV 权限、安装升级、暂停恢复、死循环停止及超时。桌面测试运行真实 LVGL/FLUX
并输出 PNG；显示、触摸、音频和无线硬件是测试替身。截图不是实机照片。
RISC-V AOT 另已完成上述 P4 v3.2 实机检查，仍不代表全部应用与硬件功能验收通过。

需要接板确认：供电/复位、屏幕 ID/时序/颜色/撕裂、触摸方向、SD 稳定性、
C5 链路/联网/TLS、扬声器和麦克风、实际内存余量与性能。

软件限制：当前单个 RGB565 surface；PANEL_CANVAS 提示回退到普通 guest 缓冲；
帧事务控制返回 ENOTSUP，由 SDK 走已有批次协议。方向固定，未实现旋转。
单 scanout 缓冲不保证无撕裂。摄像头、IMU、键盘、蓝牙未启用，能力查询为 false；
对应可选设备接口返回 ENODEV/空结果，不伪造传感器或采集数据。
这些限制尚未解除前，不应对外宣称完整、已验收的 Aster 硬件目标。

## Wiring and panel settings

Panel timing/initialization comes from the display archive example. GPIOs
are corrected against the supplied development-board User Guide dated
2026-09-10, page 6 (baseboard V1.5 schematic), which differs from that
display example. No hardware has been tested.

- ST7102: 480×480 RGB565; 2 DSI lanes at 620 Mbps; DPI clock 30 MHz.
- Horizontal back/pulse/front porch: 140/2/120; vertical: 8/4/200.
- DPHY power: internal LDO channel 3, 2500 mV.
- Backlight: GPIO20, active high. LCD reset: GPIO5, active low.
- ST7123 touch (inferred from the reference example): I2C1 at 100 kHz,
  SCL GPIO8, SDA GPIO7, reset GPIO6, interrupt GPIO21.
- SD: slot 0, CLK43/CMD44/D0=39/D1=40/D2=41/D3=42, detect GPIO45.
- C5: SDIO slot 1, CLK18/CMD19/D0=14/D1=15/D2=16/D3=17; reset54,
  boot6, according to the core-board connection diagram. GPIO6 is shared
  with touch reset; initialize touch before any C5 reset/boot sequence.
- ES8311: SDA7/SCL8, MCLK13/BCLK12/WS10/TX9/RX11, amplifier enable53.
  The image probes ES8311 and provides playback/recording; no acoustic test has been run.
- 16 MB flash and 32 MB PSRAM match the module specification. PSRAM
  speed and P4 silicon revision still need physical verification. The
  display reference uses 200 MHz PSRAM and pre-v3 P4; current core-board
  documentation describes v3.x. The build tool now defaults to v3.x
  with minimum revision 3.0. Use `--revision pre3` for older chips; its
  build and sdkconfig are isolated under `build/osptek-p4-pre3/`.
  A pre-v3 image is not a v3.x image.

GPIOs are configurable under `Aster OSPTEK P4 baseboard V1.5 wiring` in IDF
menuconfig. DSI uses the chip's dedicated pins. LCD reset is released
before the vendor driver's initial DCS ID read. Touch reset is held high
before LCD initialization because the reference uses a TDDI panel.

The diagnostic initially draws six vertical bars (red, green, blue, white,
black, yellow). If `/SD/app.wasm` exists, the VM takes ownership of drawing
and touch polling. Touch failure leaves display-only apps available. A successful compilation is not evidence of
a working physical display.


## Attribution

Display initialization and touch sequencing are adapted from the supplied
OSPTEK example, copyright 2026 OSPTEK, CC BY 4.0:
https://github.com/osptek . Source archive:
`display-tft-3.95inch-480x480-main.zip`.
Changes replace the LVGL/Modbus demonstration with RGB565 diagnostics and
Aster display/input callbacks, expose GPIO configuration, and adapt the
driver for IDF 6.0.
The vendored ST7102 driver retains its individual Apache-2.0 notices.

GPIO source: `esp32-p4c5-module-dev-board-main.zip`, English User Guide
20260910 page 6 / Chinese guide page 6. Core SDIO source:
https://github.com/osptek/esp32-p4c5-core-board ,
`images/c5-p4-connection.png`. The archived standalone V1.3 schematic and
the V1.5 schematic embedded in the newer guide are separate revisions.
