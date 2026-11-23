# 架构与移植边界

## 仓库和契约

固件和 SDK 各自是独立 Git 仓库，通过版本化 ABI/硬件描述发布物和设备协议关联；没有源码路径、Git 子模块、外部头文件软链接或构建调用上的相互依赖。

固件拥有设备功能、系统资源和运行时。SDK 拥有 APP API 副本、应用库、工具链与部署工具。固件的 `tools/export_contract.py` 输出 ZIP，SDK 的 `tools/import_contract.py` 显式校验导入；正常编译只读取各自仓库内容。

接口使用 `aster_*` 和固定宽度 ABI 类型。命名空间、产品身份和固定宽度标识都已改为 Aster：ASTP 包、ASTI 图标、ASTR 资源块、ASTERFS 文件传输协议。旧 MJX 拼写一律不再接受（改名前的包和固件互相拒绝），旧 APP 需重新编译。当前 ABI 为 4.6.0。网络传输只有 `aster_net_info`、`aster_net_open`、`aster_net_io`、`aster_net_status`、`aster_net_cancel`、`aster_net_close` 六个导入；3.x 包会被拒绝。

## 公共核心与系统后端

`common/` 是 IDF 与 Zephyr 共用的内部核心，当前包含包/清单解析、HTTP/MQTT/WebSocket 编解码和字节序工具。公共代码仅依赖标准 C、ABI 契约及显式传入的端口回调，不包含 Zephyr、FreeRTOS 或 ESP-IDF 头文件。两个构建系统读取 `common/sources.cmake`，不复制公共实现。

包解析的 `aster_pkg_policy_t` 由后端提供目标架构、ABI、分配器和日志回调。因此相同解析器既可检查 P4 的 RISC-V 包，也可检查 lckfb 的 Xtensa 包。APP ABI 和安装布局保持不变。

公共文件系统、时钟、同步、设备和网络接口位于 `common/port/`。首次开机和配网界面由 `systems/shared/setup/` 复用。部分安装事务、注册表和 UI 源码仍位于 `systems/zephyr/`，ESP-IDF 通过适配层使用这些实现。[公共模块](../common/README.md)

## 内部动态内存与运行时

两款 ESP32-S3 板的内核堆为 96 KiB，位于 SRAM 尾部；其余区域交给 libc `malloc`。`aster sys mem` 分别报告内核堆和 libc 堆。WAMR、APP 线性内存和 LVGL 主池使用 PSRAM。网络传输使用六个导入：`aster_net_info`、`aster_net_open`、`aster_net_io`、`aster_net_status`、`aster_net_cancel`、`aster_net_close`。

## 硬件型号与工作模式

当前支持以下硬件型号，具体状态与能力以 `hardware/` 的公开描述和设备握手为准：

<!-- hardware-models:start -->
- `lckfb-s3`：立创开发板 ESP32-S3 实战派。
- `waveshare-35b`：微雪 ESP32-S3-Touch-LCD-3.5B，实验性板级支持。
- `osptek-p4`：鱼鹰光电 P4C5 开发板（ST7102，480×480），ESP-IDF。
- `fakepod-s31`：FAKE POD NANO PRO，ESP-IDF，460×460，支持可选系统播放器和经典蓝牙音频。
<!-- hardware-models:end -->

S3 硬件使用 `--target xtensa`，P4/S31 使用 `--target riscv`。硬件型号由固件配置选择，SDK 不接受板卡名作为编译目标。

无屏、横屏、竖屏是同一固件的工作模式，不能通过创建多个产品 ID 或编译目标实现。它们共用驱动、内存、运行时和服务。显示模式不参与 APP 包的硬件身份判断。

- `config/Kconfig`：系统、真实板卡、服务开关、默认启动模式。
- `config/aster.conf`：menuconfig 保存的本机配置；默认值来自 `default.conf`。
- `hardware/*.json`：各板卡公开属性和固件私有构建映射。
- `boards/`：设备树定义物理接线及屏幕两种硬件窗口，不按运行模式拆固件。
- `platform/`：统一 HAL 和各硬件端口，声明不依赖操作系统头文件。
- `common/`：不依赖操作系统的包解析、协议编解码及基础工具。
- `systems/zephyr/`：完整设备服务、桌面与 WAMR 适配，明确隔离 Zephyr 依赖。
- `systems/espidf/`：ESP-IDF/FreeRTOS 运行时、外设和系统服务适配；部分待提取服务仍复用 Zephyr 目录下的源码。

Zephyr 的项目 menuconfig 独立运行，配置先于设备树与 Kconfig 处理阶段读取。用户不必再传 board、target 或朝向 overlay；这些板卡共用 `tools/firmware.py` 的 build/flash 入口。鱼鹰 P4C5 和 FAKE POD 分别使用自己的 ESP-IDF 工程及工具链。[鱼鹰构建](../boards/osptek_p4/README.md) · [FAKE POD 构建](../boards/fakepod_s31/README.md)

## 运行期显示切换

`aster display headless|portrait|landscape` 向运行时工作线程提交请求。该线程拥有 LVGL 和 APP 生命周期，负责排空显示提交、切换硬件窗口、同步 UI 尺寸并重建桌面。活动 APP 占用屏幕期间拒绝手动模式切换；APP 自己的方向声明继续使用生命周期内的方向借用机制。

无屏模式关闭显示输出与触摸能力，保持 shell、存储、网络和非图形应用服务。显示能力根据运行期状态报告，不能只看编译选项。没有可用显示设备时，启动路径仍初始化输入和其余服务。

当前硬件的公开描述给出硬件支持的能力上限；固件通过配置可以裁减服务，并通过运行模式暂时关闭显示能力。SDK 构建检查 CPU 平台契约和能力名称；具体板卡是否具备所需能力由固件在启动 APP 时检查 `supported_boards` 和 `requires`，APP 运行时查询实际能力。

## 新增硬件或系统

同架构的新板沿用现有应用包格式和 SDK。Zephyr 板通过设备树、硬件选择和 Kconfig 注册。ESP-IDF 板在独立 IDF 工程的 `aster_board.cmake` 里选择已有适配器，在板目录提供接线与可选服务。需要新器件时实现相应接口。商店分发该板的初始化资源时，还需发布匹配其设备 ID、架构和屏幕的配置。[适配示例](platform-adapter-api.md)

`systems/posix/` 只是用于在 Linux x86_64 上验证核心 ABI 和 APP 生命周期的开发工具，不列入硬件清单，不是第二款固件，也不宣称完整硬件模拟。

固件内部不依赖 SDK。应用只根据 ABI 查询能力、屏幕几何和音频参数，不包含操作系统头文件、厂商结构体或引脚信息。SDK 编译与部署选择 CPU 平台，具体硬件由固件配置选择；屏幕尺寸、内存上限和实际能力属于设备运行环境。

## 系统资源与发布

字体和桌面资源属于固件发布物。公开字体度量随契约发布，SDK 排版助手读取固定副本；SDK 不运行固件资源生成器。固件构建检查实际 CPU、AOT、能力与物理显示几何，避免发布描述超出硬件支持范围。

APP 名称、介绍与图标来自安装包元数据和资源。桌面的动态 APP 文字使用通用 SD 字库，固定系统界面可以使用已有内置 UI 字体；安装、改名或升级 APP 不要求将其文字加入固件字体，也不要求重新编译固件。桌面的字形和光带缓存属于固件渲染层，不改变 APP 的字体接口或资源边界。

设备固件只运行匹配 CPU 的 AOT。安装缺少 AOT、CPU 不匹配或加载失败时直接报错，并保留已安装的 AOT 文件。模拟器和 POSIX 主机测试程序执行 WASM。

## 原生字号与字形缓存生命周期

SD 资源提供源字体全覆盖的 12/14/16/18/20/24/28/32/40/48px 字库；固件与 SDK 度量由同一 `faces.json` 生成。请求字号选最近原生字库，字形不做运行时像素缩放。桌面固定文字和动态 APP 名称均通过该加载器，APP 不需向固件添加字符。

12–20px 字库使用最终 ppem 的 FreeType 灰度自动网格微调，24px 及以上保留 4 倍采样后缩小。两条路径共用独立的 1 倍度量通道，文字进宽、行高与基线不因像素生成方式改变。FLUX 选中光带在停稳时为零旋转、原生比例，邻居保留倾斜与缩放；常见 48px APP 图标按 48px 绘制，避免先放大图标再变换整张卡片。字库更新需连同固件发布到 SD 卡，单独刷固件不会替换卡上的旧字形。

开机仅注册字号和加载小型区间索引。每个被请求字号独立申请 PSRAM 缓存，位图预算 `clamp(px*px*48, 8192, 49152)`，最多 192 个字形，附加动态索引和 A8 scratch。完整字形记录与位图按需从 SD 读取，LRU 淘汰并压紧空间，不加载整套度量表，也不整批清空。

桌面不保留运行中 APP 不需要的字形缓冲：隐藏桌面后、APP init 前开始缓存会话并预热按实际屏幕选择的 body/title 字号，APP 额外请求的字号独立申请。启动失败和正常/异常退出均释放会话缓存。返回桌面再申请和预热，稳定字体对象与小型区间索引保留。缓存创建、使用和释放都在共享 WAMR/LVGL worker 串行发生；shell 状态读取稳定槽位的统计快照，不解引用可释放字形存储。

冷字形目前同步读 SD，启动预热减少常见场景的冷读；大量新字符首次显示仍有加载成本。`aster sys fonts` 报告每档资源覆盖和缓存占用。Native USB 的 stop-and-wait 文件传输保持 512-byte ACK 协议，等待下一 USB packet 时短轮询并让出 CPU，避免每个 64-byte packet 产生 1ms 等待。

发布前运行 `python tools/check_docs.py`。契约导出前同样执行该检查。

安装与升级见 [安装与恢复](package-installation.md)。回调期限见 [运行时恢复](runtime-recovery.md)。


### 芯片、开发板与编译目标

芯片 `esp32p4`、`esp32s3`、`esp32s31` 只表示 SoC，不能作为开发板唯一标识。
板级公开描述保留稳定 `id`，另提供实际运行时的 `board_id`、明确的板卡 `name`、`soc`、显示和内存等参数。
`osptek-p4` 专指鱼鹰光电 P4C5 + ST7102 480×480 组合，不是所有 ESP32-P4 板。
其现有运行时/公网配置标识 `osptek-p4-st7102` 保持兼容；`lckfb-s3` 专指立创 ESP32-S3 实战派。

IDF 的每个板级工程通过自身 Kconfig 声明身份与屏幕参数。共享 UI 不再根据“是不是某颗芯片”推断开发板或分辨率。
背光 GPIO/PWM 实现由该板注入 IO 回调，音频板级驱动和外部 C5 的 SDMMC 共享补丁按实际板型选择。
固件在 `hardware/` 和 `boards/` 注册新板。SDK 默认按 `targets/` 的架构编译。新增同架构且 AOT 配置兼容的板时，应用可沿用现有构建，无需先把板名登记到 SDK。专用应用可以用 `supported_boards` 限定运行时板型。


### 可选电源适配

电池、屏幕开关、CPU 休眠/唤醒、重启/关机与实体按键使用 `common/port/aster_power.h`。
硬件操作表及系统生命周期表可继承默认实现并覆盖个别方法；缺少后端的能力明确返回不支持。
实现范围、调用顺序和当前验证记录见 [电源与按键适配](power-adaptation.zh-CN.md)。
