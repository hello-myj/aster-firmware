# Waveshare ESP32-S3-Touch-LCD-3.5B

新增的项目内 Zephyr board：`waveshare_esp32s3_touch_lcd_35b/esp32s3/procpu`。
Aster 硬件型号：`waveshare-35b`；SDK CPU 目标：`xtensa`，ABI 3.7.0。V1/V2 官方说明程序通用。
当前状态：实验性板级支持；已在微雪真机验证刷机、USB shell、SD 挂载和 320×480 主页显示。当前默认固件不启用 QMI8658，不声明 accelerometer/gyroscope 能力；历史 IMU 采样测试不代表当前配置支持 IMU APP。其他外设与 APP 行为仍以实机验收为准。

依据：[官方资料入口](https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.5B/Resources-And-Documents/)、
[官方示例程序](https://files.waveshare.net/wiki/ESP32-S3-Touch-LCD-3.5B/ESP32-S3-Touch-LCD-3.5B-Demo.zip)
中的 `ESP-IDF/01_factory/components/esp_bsp`。显示初始化寄存器表来自同一示例。
新增设备树与板级驱动位于本仓库。共享 Zephyr checkout 还需要应用
`patches/aster_zephyr_v4.4.2_qspi.patch`（在现有基础补丁之后），
修正 ESP32 SPI 驱动截断四线模式标志的问题。
USB 调试模式的串口控制台还需要
`patches/aster_zephyr_v4.4.2_usb_serial_console_batch.patch`（批量化 TX，见下文"控制台日志开销"）。
PSRAM 直接 DMA 补丁仅供后续诊断，当前板级配置明确关闭直传，使用内部 RAM 中转后 GDMA 发送。详见 `patches/README.md`。

## 构建与烧录

从固件仓库根目录，在 Zephyr Python 环境中执行：

```bash
python tools/firmware.py build --config boards/waveshare_35b/selection.conf --build-dir build/waveshare-35b
python tools/firmware.py flash --config boards/waveshare_35b/selection.conf --build-dir build/waveshare-35b
```

镜像：`build/waveshare-35b/zephyr/zephyr.bin`（通过 west flash 烧录，不是含 bootloader 的合并镜像）。
独立配置和构建目录保留原 `config/aster.conf` 与 `build/firmware`。
也可在 `python tools/firmware.py menuconfig` 的 Hardware model 中选择该板。
摄像头是可选外接模块，目前没有选择传感器驱动，配置关闭且不声明 camera 能力。

SDK 可使用 `aster build APP_DIR --target xtensa --aot`，包位于 APP 工程的 `dist/xtensa/`；部署示例中的路径以 APP 工程为当前目录。屏幕必须通过 `aster_display_get_info()`
读取实际尺寸，避免将旧板 240×320 写死在 APP 中。

## QSPI 预拷贝与调频验证（2026-10-14）

默认仍为 40 MHz、同步内部 RAM 中转。新增像素时钟和内部 RAM 预拷贝选项
默认关闭加速，立创板配置不变。启用预拷贝须按 `patches/README.md` 顺序应用
补丁，最后应用 `aster_zephyr_v4.4.2_tx_prefetch.patch`。

内部 RAM 中转可使用两个最多 8192 B 的 band：当前 DMA 发送一个 band 时，CPU
预拷贝下一个，等待完成后才交换。数据缓冲合计最多 16384 B，替换原最多
16384 B 的单缓冲；另有少量状态开销。不增加整屏 DRAM 缓冲，不启用 PSRAM
直接 DMA。短包、接收、非连续缓冲或分配失败沿用原路径。

同一实机、诊断镜像、测试包下，四种时钟/预拷贝组合共验证 3072 帧，APP 错误
为零，软件截图像素一致。40 MHz/关闭预拷贝与 80 MHz/开启预拷贝相比：稀疏
命令场景绘制/提交均值 19.99 → 9.02 ms，复杂完整帧 69.72 → 58.39 ms。
这是绘制/提交耗时，不能视为实体屏 FPS。两种中转方案观测 DRAM heap 峰值
均为 41764 B；测试未覆盖所有并发负载。

**80 MHz 日常版实体画面验收失败：用户反馈图形有很多毛刺。** 软件截图来自
发送前缓冲，不能证明线上传输正确。80 MHz 不作为可用日常配置；40 MHz＋预拷贝对比版则由用户确认“毛刺消失，画面正常”；原国际象棋、2048、
Gfx Show 启动及短时运行通过。可选配置为 `qspi-prefetch.conf`：

```bash
python tools/firmware.py build --config boards/waveshare_35b/qspi-prefetch.conf --build-dir build/waveshare-35b-prefetch
python tools/firmware.py flash --config boards/waveshare_35b/qspi-prefetch.conf --build-dir build/waveshare-35b-prefetch
```

40 MHz 下仅开启预拷贝，受控测试稀疏命令 19.99 → 16.74 ms（降低约 16.2%），
复杂完整帧 69.72 → 66.14 ms（降低约 5.1%）。该 40 MHz/双 8 KiB/48 KiB 配置已通过十分钟图形、音频和资源并发及实体确认；IMU 保持禁用，音频实听不在此次范围。详见 `docs/validation/graphics-g5-2026-10-14.md`；稳定回退
为 `usb-debug.conf`。

### 双 16 KiB band 与 80 KiB 内核堆对照

可选实验配置 `qspi-prefetch16.conf` 将内核动态堆从 48 KiB 增至 80 KiB，启用
`SPI_ESP32_TX_PREFETCH_LARGE_BANDS`，仍保持 40 MHz、RGB565 和内部 RAM DMA。
两个 16 KiB band 合计 32 KiB，整屏分块由 38 次降至 19 次；每个 DMA 命令仍
不超过原 16 KiB ceiling。分配失败、低预算和非适用输入保留回退路径。

相同 80 KiB 堆、时钟和测试包，两个场景各三轮、128 帧，共 1536 帧，软件像素
一致、APP 错误为零：双 8 KiB / 双 16 KiB 的稀疏绘制均值为 16.49 / 16.25 ms；
复杂完整帧为 63.00 / 63.40 ms。DRAM heap 观测峰值为 41764 / 58148 B。
该实验未显示明显、普遍的刷新收益，不能因分块减半就宣称帧率翻倍；常用配置
仍推荐较省内存的双 8 KiB，双 16 KiB 保留为可选实验。按用户当时选择，G5期间的 `qspi-prefetch.conf` 曾恢复48 KiB内核堆＋双8 KiB；当前堆配置见下节。
上述双 8 KiB 性能对照使用的是独立的 80 KiB 测试堆，不能视为当前 48 KiB 配置数据。

本地 Zephyr 补丁将内核堆置于静态区之后的 SRAM 尾部，链接报告静态 DRAM
173072 B 不包含这块堆。预留 80 KiB 内核堆后，libc 初始分配区域约 140.7 KiB，
比 48 KiB 配置减少 32 KiB；`aster_dram_alloc` 等通过 libc 分配，仍需并发压力
验收。该历史实验时源码默认堆和LCKFB配置未变。用户已确认双 16 KiB 版的画面、触摸、上拉返回确认及取消均正常；此为短时
基础验收，长时并发压力仍待完成。软件截图不能代替屏幕观察。

### 当前内核堆与libc统计

两板基础配置及本板常用`qspi-prefetch.conf`现将内核堆设为96 KiB，为后续网络服务预留空间；本板继续40 MHz、RGB565、双8 KiB预拷贝，未切换双16 KiB实验。上述G5数据仍是当时48/80 KiB配置的历史结果，不改写成96 KiB实测。

`aster sys mem`分别输出内核`dram heap`和libc `usable/peak/used/free`。项目的SRAM尾部先扣除内核堆预留，再交给libc；不能将`_heap_sentry - _end`全部称为libc容量。扩容后本板libc arena约125 KiB，最终以新构建和运行时统计为准。本轮微雪仅编译，尚未实机补测；详情见[网络服务实施方案](../../docs/network-services-implementation.zh-CN.md)。

## 外设连接

- 屏幕：AXS15231B，原生竖屏 320×480；SPI2 QSPI 40 MHz，CLK=5、D0..D3=1/2/3/4、CS=12。
  独占 SPI2，以单线命令头和四线像素传输保持同一 CS 周期；像素为线缆顺序 RGB565 大端，每像素 2 B，屏幕 COLMOD(0x3A) 写入 0x55。
  厂商标注 262K 色是面板能力；当前固件采用 65,536 色输入，而非 RGB666。
  QSPI 采用全宽顺序流：首段 RAMWR，后续段 RAMWRC，不使用 RASET 任意行窗口。
  驱动在 PSRAM 保留 307200 B 画面缓存，局部或乱序矩形更新后重传整帧，避免内容反复写到顶部。
  寄存器表是微雪该屏幕的专用配置，不能用于任意 AXS15231B 模组。
- 背光：GPIO6，LEDC 5 kHz PWM，高有效；亮度 API 的 0..255 映射为 PWM 百分比。
- I2C0：SDA=8、SCL=7，400 kHz；触摸 0x3b、PCF85063 0x51、AXP2101 0x34、ES8311 0x18。QMI8658 0x6b 仍在总线上，当前固件不启用。
- SDMMC：1 位，CLK=11、CMD=10、D0=9，初始上限 10 MHz；FAT 卷名 SD，挂载 /SD:。
- 音频：ES8311 同时提供 DAC/ADC；MCLK=44、BCLK=13、WS=15、SoC TX=16、SoC RX=14。
  沿用 Aster 16 kHz/16 bit/stereo PCM 传输接口，不探测本板不存在的 ES7210。
- BOOT：GPIO0，沿用退出 APP 的按键码。PWR 属于 PMIC，当前保留芯片自身行为，未接入 Aster 输入事件。
- PMIC：上游 AXP2101 MFD/调压器驱动，初始化 DCDC1/3/5 和 ALDO1..4 为官方示例的 3.3 V；
  PMIC 优先级 80、调压器 86、触摸/IMU/RTC/屏幕 90。充电参数、电量 UI、关机 API 尚未移植。

## 串口与 APP 部署

板载 Type-C 的 USB Serial/JTAG 用于 west/esptool 烧录和 Aster 文件服务（通常 /dev/ttyACM0）。
Shell 使用独立 3.3 V USB-TTL：板 TX=GPIO43、板 RX=GPIO17、GND；交叉连接，115200 baud。
GPIO44 是 ES8311 MCLK，因此不能沿用 ESP32-S3 默认 UART0 RX=44。
固件 `hardware/waveshare-35b.json` 记录板级部署默认值；SDK 使用 `targets/xtensa.json`，通过 `--port` 指定实际设备串口，双串口配置可用 `--console` 指定命令端口。
标准模式没有外接 USB-TTL 时可烧录和传输文件；下方 USB 调试模式可以仅使用板载 USB 获得 shell。

仅接板载 USB 调试时可使用独立配置：

```bash
python tools/firmware.py build --config boards/waveshare_35b/usb-debug.conf --build-dir build/waveshare-35b-usb
python tools/firmware.py flash --config boards/waveshare_35b/usb-debug.conf --build-dir build/waveshare-35b-usb
```

USB 调试模式将 console/shell 切到 USB Serial/JTAG；该串口同时支持日志、shell 命令和文件传输。
SDK 探测到 `serial=shared` 后自动在同一端口安装和启动 APP，例如：

```sh
aster deploy dist/xtensa/aster.hello.ast --target xtensa --port /dev/ttyACM0 --run
```

无需为传包切换镜像。LCKFB 的 CH340 shell 同样支持该功能（`/dev/ttyUSB0`）。
旧的独立 USB 文件服务保留兼容；同一时间只允许一个文件传输。
上传前关闭独立串口监视器，上传完成后可在同一端口恢复监视。

将现有字体资源与 APP 放到 FAT SD 卡中，系统字体按固件 README 的资源部署方式准备。
共享串口配置由 shell 统一接收并在传输期间切换处理器；不要另开独立文件接收线程与 shell 争抢同一 UART。

## 显示方向与 launcher

AXS 驱动保持原生 320×480 窗口。横屏 APP 由 Aster 软件旋转为 480×320，触摸使用相同逆变换；
退出时清除旋转恢复竖屏。横屏会失去原生 panel-canvas/直接发送的部分优化，需要实机测帧率。
LVGL pool 配置为 1024 KiB，以容纳完整 307200 B 截图和光带旋转/缩放的临时图层。

该板启用 `CONFIG_ASTER_LAUNCHER_FLUX`：原生 LVGL 实现用户提供的
`aster-flux-v4-20261002-191041.zip` 中 FLUX Rev.04 的 320×480 布局。
主页采用斜向彩色光带、原色 SDK 图标、纵向拖动/惯性吸附、横向拉开启动和底部拉环。
频段页按实际安装应用分组，每个频段保留选择位置；调音台连接真实亮度、音量和 Wi-Fi/个人热点设置。
长按中央应用可确认卸载，APP 内从最右 22 像素左拖超过 65 像素可返回主页。
应用列表由安装注册表生成，未知应用归探索；文件、设置、系统状态和时钟为原生系统入口。
蓝牙尚无设备服务，调音台显示“未启用”；电量不显示网页演示百分比。
图层渐变/阴影简化以控制软件绘制开销，动画按实际经过时间推进。
launcher 使用实际字号下 FreeType hinting 生成的内置 A8 字体，不再先放大栅格再缩小；
英文小字至少 12 px、中文小字至少 14 px，并提高辅助文字对比度。
未知应用名称仍回退 SD 字体；
系统图标按实际显示的 11–40 px 尺寸直接栅格化，避免先生成 24 px 再缩小；
内置字体保留 A8 灰度抗锯齿，只轻微压缩最淡字边的覆盖度过渡，提高
笔画对比度。光带文字、图标与卡片边缘均保留双线性过滤，避免最近邻
旋转造成锯齿。卡片四点采样按 RGB565
各通道一次加权、四舍五入，采样位置采用每轴八位小数（256 级），
减少分两次插值和低精度采样位置的截断偏差。
旋转/缩放保留双线性过滤和边缘抗锯齿。
上下快拨使用平滑采样的速度估计，松手后采用按速度决定距离与时间的
惯性减速（最长 1.8 秒），跨列表首尾保持拨动方向；慢滑就近吸附，
再次按住可中断惯性，停止滚动的第一次轻点不会误打开 APP。
卡片轻点启动要求按下/松开命中同一卡片、移动不超过 6 px、时长不超过
300 ms；卡片横向及斜向滑动不会触发启动，拖动启动仅用于底部拉开控件。
触摸队列保留按下/松开边缘与移动中的最大偏移，避免短滑和滑出再返回
在两次 UI 轮询之间被压缩成点击。
每轮输入合并卡片重绘，结束后再保存最终选择，避免滚动时写 SD。
仅保留五条可见/相邻光带缓存；跨过一个应用时复用其余四条纹理。
文字、原色图标和底色先合成为 RGB565 光带，拖动时使用融合旋转采样/透明度混合，
不反复创建标签图层。LVGL 使用两个完整 PSRAM 帧缓冲和 DIRECT 模式，
仅绘制损坏区域，最后一个区域完成后从第零行发送一次整帧。
微雪 UI 帧按 64 B 对齐，通过内部持久帧接口省去屏幕驱动的每帧复制；
SPI 驱动按块搬入内部 RAM，再由 GDMA 发送到屏幕，不直接从 PSRAM 取数。
LVGL 在 DMA 完成前保持刷新中的状态，通过 flush_wait_cb 等待后再同步/复用缓冲。
不能在提交时提前调用 flush_ready。截屏在同一 UI worker 内排队并先等待发送完成。
此接口不提供给 APP；普通 display_write 保持复制语义，局部更新先合成完整画面。
截图复用当前显示帧，避免再分配/复制一份整屏快照。
板级缓存配置为 32 KiB 指令缓存、64 KiB 数据缓存和 64 B 数据缓存行。
选择位置等设置在手势和吸附动画结束后合并保存，退出 launcher 时立即保存。
立创与微雪都使用这一套流光桌面。立创按自己的分辨率缩放布局。

## 实机验证清单

构建成功只验证设备树、驱动编译与 ABI 目标声明，不代表外设已通过实机验证。
2026-10-02 的 USB 调试验证已完成字体加载、ES8311 芯片探测、IMU 立方体安装与启动。
四线模式修正后像素传输约 8–11 MB/s；运行中仍出现过可恢复的 IMU 读取超时。
用户反馈 IMU 画面仅显示在顶部且退出/重启后残留；已修正分段 RAMWR 重置写指针问题，
用户已确认修正版完整显示主页及底部分页点，没有旧画面残留；APP 显示与触摸继续验证。
2026-10-03 优化验证：同一微雪 USB 调试板，320×480、QSPI 40 MHz、
已加载 SD 字体，注入上下拖动。在优化初始基线中 LVGL 帧处理均值为 233–241 ms；
DIRECT、缓存复用和融合绘制版本的分段均值为 73–94 ms，跨条目峰值仍约 134 ms。
随后加入原生字号字体/图标、双线性抗锯齿、持久 UI 帧直接 DMA、
融合绘制的平坦区域跳过，以及实验性的绘制/发送重叠。相同 16 步、80 ms 间隔的往返拖动中，
该实验版本分段帧处理均值为 44.7–46.5 ms，峰值约 50–53 ms；不包含启动首帧。
提交均值约 0.55 ms；连续 64 次整屏发送总计 1.090 s，即每次约 17.0 ms。
帧处理统计包括 LVGL 处理/提交耗时，不等于整机持续刷新率；尚未达到稳定 30 FPS，
剩余开销主要是 CPU 对旋转/缩放光带的绘制，仍需使用者判断触摸跟手程度。
主页截图的 320×480 像素流已通过 CRC 检验；它验证提交的缓冲内容，不能替代实体屏花屏检查。
固件 19 项测试通过，覆盖纹理缓存/抗锯齿、字体位图约定、DMA 对齐门槛和持久帧转局部更新。
微雪标准/USB 调试、原 LCKFB 构建已通过。
连接微雪板及外接 shell 后：

1. 上电检查 PSRAM、SD 挂载、AXP2101、屏幕初始化日志；`aster sys info` 的 target 应为 waveshare-35b。
2. `hw display colorbar` 检查 RGB 色序；触摸四角及中点，确认坐标、按下/抬起和滑动事件。
3. `hw imu` 检查六轴采样，运行 IMU 立方体；启动横屏 APP 再退出，确认 launcher 恢复 320×480。
4. 检查 PWM 亮度、播放和录音、Wi-Fi 扫描/连接、个人热点；同时监测内存与异常日志。
5. 在实际卡上确认字体、安装、卸载和断电后文件系统状态。

2026-10-03 后续实体屏反馈：滑动时出现横条/矩形区域跳动；此前 CRC 截图
不能验证 DMA 传输及屏幕扫描的稳定性。已撤回提交即 flush_ready 的优化，
恢复 DMA 完成后才能同步/复用缓冲的规则，但用户反馈仍乱跳。
第二个对比版保留同一绘制路径/字体/抗锯齿，关闭 PSRAM 直接 DMA，使用内部 RAM
中转 + GDMA；用户确认“不再乱跳”。该中转路径已设为默认，保持完成等待。
中转对比版与最终默认版帧处理均值约 48–52 ms，尚未达到稳定 30 FPS；此前直传约 17 ms 的发送
统计不代表当前默认路径；最终默认版连续 64 次发送合计 1.317 s，约 20.6 ms/帧。
底层缓存/外部内存传输原因还未完全定位，不应重新默认启用直传。

字体后续优化：补齐原生 12 px 字体，英文辅助字至少 12 px、中文至少 14 px；
使用目标 ppem 下的 hinting 和完整 A8 灰度，取消 launcher 字体的 4x 栅格后缩小及
4bpp 中间量化。增加辅助字对比度并调整底部标语长度。主页、频段页、调音台
320×480 截图均通过 CRC，已检查字号放大后的布局；用户已确认实体屏“小字和大字都明显改善”。
SDK/SD 字体格式及 APP 字体 ABI 未变，继续保持内部 RAM 中转 + GDMA 默认配置。

### FLUX 交互音效

Lightgrain 音效位于 `systems/zephyr/ui/launcher/flux-sounds/`，保留原始 WAV、生成脚本和触发说明。
`python systems/zephyr/ui/launcher/flux-sounds/bake.py` 解析 WAV 数据块，将 22050Hz 单声道转换为原生 16000Hz 单声道，素材整体增加 18dB（8 倍幅度，原始峰值约 -21～-23dBFS，转换时检查不发生削波），生成 `flux_sounds.inc`；播放时复制到左右声道并叠加到音频 worker，不在 UI 线程访问 SD 或等待声音播放结束。

- 卡片选中变化播放 tick：慢速至少相隔 95ms，超过 650px/s 时至少 160ms，并随速度降低增益；丢弃限流事件。
- 展开/返回应用播放 open/close；打开分类或调音台播放 tap，关闭播放 close，确认分类播放 tune。
- 静谧、音量为零立即取消 UI 声音；退出静谧、切换动效播放 tap，音量滑杆不逐次发声。
- UI 使用独立的一路短音频，不占用 APP 的四个音效句柄。打开/返回声优先于 tick；交给 APP 时释放纯 UI 音频会话，避免 APP 的录音/同步播放被判忙。

### APP 上拉返回确认

沿用已有上拉识别规则：从底部 28px 区域开始，上移至少 24px 且以纵向移动为主；识别后先显示“返回主页？”确认框。
点击“取消”或遮罩恢复原应用画面，只有点击“返回主页”才结束 APP。确认框显示期间暂停 APP 事件派发并拦截触摸，已暂停的 APP 在取消后仍保持暂停。
此功能由固件处理，已有 SDK APP 无需改动或重新安装。
