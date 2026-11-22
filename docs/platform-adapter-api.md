# 平台适配接口 v1

此接口属于固件内部，版本独立于公开 APP ABI。鱼鹰光电 P4C5 和 FAKE POD NANO PRO（ESP-IDF）、立创实战派（Zephyr） 和桌面后端使用同一组 C 契约；外设引脚、厂商驱动和操作系统对象由适配器持有。

## 四个边界

- `common/port/aster_device.h`：显示、帧缓冲、方向和坐标、触摸与按键、音频、IMU、相机、Wi-Fi。`aster_device_ops.def` 列出已有硬件操作。未提供的状态操作返回不支持，布尔查询返回 false，指针查询返回 NULL；void 通知为空操作，调用可选 void 诊断前查询 `aster_device_has()`。驱动原有返回值保持兼容。
- `aster_os.h`：内存区域、时钟、睡眠、文件/目录/容量、递归互斥锁、计数信号量、任务启动与等待。公开句柄只含不透明指针；Zephyr、FreeRTOS、VFS 结构体不穿透接口。返回值为负 errno；读取、写入返回实际字节数。超时单位是毫秒，`ASTER_OS_WAIT_FOREVER` 表示无限等待。任务优先级 0 最高，栈大小单位为字节，core=-1 不绑定 CPU；不支持的亲和性必须报错。
- `aster_services.h`：设备资料和实际能力、应用注册表快照、启动/安装/卸载入口、代码目录、KV、存储状态和日志。注册表返回调用方拥有的快照，避免公共 UI 持有原生注册表中的易失指针。APP 启动/安装入口继续沿用原服务的结果约定。
- `aster_network.h`：41 项异步 TCP/UDP、HTTP(S)、MQTT、WebSocket 和网络事件操作。接口保留 owner、代际句柄、EAGAIN/EOF、部分读写和关闭归属的原有规则。公共代码不获取 socket/TLS 对象。证书校验必须保留，不能在缺少 TLS 实现时降级为 HTTP。

所有接口头只依赖标准 C、公共类型及 APP 契约头。公共 UI 的布局尺寸来自运行时显示适配器，不再引用设备树。当前 UI 支持的逻辑高度下限为 240 像素；更小的屏幕需要先调整下拉面板和手势布局。

电源与媒体是独立的可选服务。需要读卡器、屏幕唤醒或关机时，在板上注册电源操作及相应的系统收尾回调；带独立 DAC 的板可注册音频输出和后台媒体服务。Launcher 和 APP 根据已注册的能力显示操作。[电源接口](power-adaptation.zh-CN.md) · [媒体适配](fakepod-s31-hifi-design.zh-CN.md)

## 复用与覆盖

每个模块都有 `derive(out, base, overrides)`、`bind(ops)` 和 `seal()`。这相当于 C 中的基础实现与方法覆盖：覆盖表的 NULL 成员继承基础方法；要禁用继承的方法，应在派生后显式清空该成员。版本、结构体大小与名称在派生和绑定时校验。

```c
static aster_device_ops_t board_ops;
const aster_device_ops_t overrides = {
    .size = sizeof(overrides),
    .version = ASTER_DEVICE_API_VERSION,
    .name = "my-new-board",
    .backlight_set = my_backlight_set,
};
int error = aster_device_derive(&board_ops, &family_ops, &overrides);
if (!error) error = aster_device_bind(&board_ops);
/* Check error before starting any worker. */
```

表及名称必须覆盖整个系统生命周期。只能在启动工作线程之前绑定；`seal()` 后再次绑定返回 EBUSY，禁止带着活动句柄切换后端。默认后端直接使用常量表，不复制到 RAM，也不为接口层新建常驻线程。

句柄必须零初始化，不能复制活动句柄。close/destroy 成功后清空句柄。销毁锁、信号量前必须确认没有持有者或等待者；task_join 超时保留句柄供重试，成功才释放任务记录。文件适配器按打开/关闭申请和释放底层句柄，不保留额外常驻文件缓存。

## 现有后端

- `systems/zephyr/port/`：绑定现有 ESP32-S3 HAL；文件、时间、内存、同步和任务使用 Zephyr 实现。
- `systems/espidf/port/`：绑定 P4 显示/触摸、C5 Wi-Fi 和音频；系统对象使用 FreeRTOS 和 IDF VFS。
- `systems/shared/port/`：两端已有服务的绑定，以及复用现有 DMA 安全读写的文件适配器。

原 HAL、服务头的位置保留转发头，现有板级驱动可以逐步迁移。Launcher、字体、Wi-Fi 页面、下拉面板、启动画面和流光绘制改用新接口；首次开机 UI 也通过显示/OS 接口访问底层。网络 ABI 桥及资源下载通过网络端口访问服务。

这一步没有把内核、WAMR、全部安装事务和协议工作线程合并成一个实现。它们仍是后端实现细节，部分文件仍保留在旧目录；接口隔离与源码物理搬迁是不同的工作。首次开机协调器继续使用现有系统适配和 PSA，桌面后端接入时应提供对应服务实现，不能仅返回成功来假装完成下载或安装。

## 新设备适配步骤

1. 同芯片、同系统先复用已有后端，增加板型资料、DTS/Kconfig 或 IDF 引脚和驱动配置。
2. 通过覆盖表实现有差异的显示、触摸、音频、背光等方法，保留坐标、像素格式和输入槽位约定；未配备的外设明确报告不可用。
3. 新增系统才需要实现 OS 和网络端口。SD 挂载、驱动初始化、中断和电源时序属于板级启动代码，在公共服务启动前完成。
4. 提供实际屏幕尺寸、内存、CPU/AOT 目标和能力；APP 仍按 CPU 架构构建，新增接口不会让 Xtensa 与 RISC-V AOT 互通。
5. 运行端口测试、目标构建，再实测字体、Launcher、下拉面板、商城下载安装和 APP 返回。

## 音频与外接 Wi-Fi 的板级配置

ESP-IDF 在发现组件依赖和正式构建时都读取当前板的 `boards/<板>/idf/aster_board.cmake`。在板目录选择适配器、声明源文件和引脚。以采用 ES8311 和原生 Wi-Fi 的新板为例：

```cmake
set(ASTER_BOARD_AUDIO_ADAPTER "${ASTER_IDF_ADAPTERS}/es8311_i2s/adapter.cmake")
set(ASTER_BOARD_AUDIO_SOURCES "${CMAKE_CURRENT_LIST_DIR}/port/audio_wiring.c")
```

在 `audio_wiring.c` 实现 `aster_es8311_wiring` 和功放使能 GPIO；无功放控制脚时后者用零初始化。鱼鹰光电 P4C5 提供了完整接线示例。FAKE POD NANO PRO 选择 `cs43131_i2s_pdm`，由接线表提供 I²C 地址、复位、I²S 端口与时钟、PDM 麦克风引脚。该适配器使用 24.576 MHz DAC 晶振。更换音频芯片时，可提供自己的 `ASTER_BOARD_AUDIO_SOURCES`，按 `audio_codec.h` 实现初始化、启用、音量及探测方法，芯片需要时再实现采样率切换。传输接口由 `audio_port.h` 定义。板级依赖通过 `ASTER_BOARD_AUDIO_REQUIRES` 声明。

系统播放器通过 `ASTER_BOARD_MEDIA_PLAYER` 选择，需由板级驱动绑定有线输出端点。频谱通过 `ASTER_BOARD_AUDIO_SPECTRUM` 选择。经典蓝牙音频另需设备名 `ASTER_BOARD_BT_AUDIO_NAME` 和 IDF 的经典蓝牙配置。Zephyr 音频使用设备树选择 I²S 和功放，可在板级 `.conf` 通过 `CONFIG_ASTER_AUDIO_CODEC_SOURCE` 指向实现同一编解码器接口的源码，默认沿用 ES8311/ES7210。

原生 Wi-Fi 板沿用 IDF 的 `esp_wifi`。外接 ESP-Hosted C5 且 SD 卡与协处理器共享 SDMMC 控制器的板，选择 `ASTER_BOARD_WIFI_ADAPTER` 为 `hosted_sdmmc/adapter.cmake`，在板级配置设置 `ASTER_BOARD_STORAGE_SLOT`，Hosted 槽由驱动的 `CONFIG_ESP_HOSTED_SDIO_SLOT` 提供。两者须使用不同槽位。板级组件依赖、SDIO 引脚和协处理器复位按鱼鹰板示例配置。SD 挂载/卸载使用 `aster_storage_sdmmc_init/deinit`。使用其他传输方式时，可提供适配脚本声明 `ASTER_BOARD_WIFI_SOURCES`、`ASTER_BOARD_WIFI_INCLUDE_DIRS` 和 `ASTER_BOARD_WIFI_REQUIRES`，需要额外配置 IDF 目标时实现 `aster_board_configure_wifi(target)`。

## APP 启动兼容性

设备维护自己的稳定板型 ID。首次定义新板时，固件的 `CONFIG_ASTER_BOARD_ID`、系统信息的 `board` 与产品资料的 `public.board_id` 应取同一个值。已有架构和 AOT 配置可以直接使用相应架构的 APP，SDK 无需登记新板。

安装保留 APP 清单的 `supported_boards` 和 `requires`。打开 APP 时，两套运行时都检查当前设备是否在声明的板型列表内，并查询此时的实际能力。缺省或空板型列表可在任何满足架构、ABI 和必需能力的板上尝试运行。比如 `["fakepod-s31"]` 表示只允许 FAKE POD NANO PRO。`optional` 只供 APP 自行判断。

## 验证入口

`aster port selftest` 显式检查外部/内部内存、时钟、递归互斥锁、信号量、任务启动和等待、设备资料、显示查询、SD 文件读写/同步/改名/目录/容量。仅创建并删除自己生成的唯一测试文件，不格式化卡，不访问已安装 APP 的资源和存档。先停止活动 APP，再执行。

主机测试 `test_device_port.py`、`test_port_modules.py` 在无 Zephyr/IDF 头文件的环境中编译真实接口层，验证参数传递、继承覆盖、无效版本、缺失能力、冻结绑定和公共 UI 的依赖边界。原有协议、安装故障矩阵、字体和 UI 测试继续运行。

2026-11-14 验证记录：P4/IDF、lckfb/Zephyr 均已构建并烧录；`aster port selftest` 均通过 0xff，OBS 商城分别成功加载 7/5 个应用目录。用户确认返回桌面、上下滑动、下拉设置、打开商城的画面、文字与触摸正常。主机回归 153 项通过，3 项按环境跳过。桌面模拟器的适配实现保存在独立的 aster-sim 仓库，不加入实机固件构建。
