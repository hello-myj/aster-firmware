# 共享首次开机引导

当前开发在 `feat/first-boot-setup`。源码不内置商城，不内置完整 APP 字体；SD 卡必须存在。
固定系统界面使用同一组 16/20/24px LVGL 字库，初始化、设置与恢复共用。
字形来自现有有许可的 cpfont 资源，由 `tools/build_system_fonts.py` 提取界面需要的字符，
生成的 C 数组位于构建目录，不额外维护一套启动专用字体。

## 分层

- `common/setup/aster_setup_core.*`：无 OS 头文件的状态与输入规则。
- `common/setup/aster_setup_json.*`：有边界的平面 JSON 编解码。
- `systems/shared/setup/aster_setup.c`：唯一的依赖检查、资源解析、续传、校验与安装流程。
- `systems/shared/setup/aster_setup_web.c` 和 `common/setup/provision.html`：唯一的手机配网服务及页面。
- `systems/shared/setup/setup_ui.c`：唯一的 LVGL 引导与进度界面，随屏幕尺寸调整布局。

共享服务通过既有 `aster_plat_*`、文件系统、HTTP、内存和线程适配层工作。
Zephyr 使用本机 API；IDF 使用现有 compat 与 connectivity/runtime 实现。
新设备需要实现相同接口和设备信息，继承公共流程；硬件差异在平台实现中覆盖，
不在状态机里增加板名判断。服务端也通过板标识和设备能力匹配资源。

## 运行行为

1. 后台检查 SD、十个完整字体及已安装的本机 AOT 商城。健康卡直接进入 Launcher。
2. 新卡显示欢迎，点击开始后启动带随机密码的热点。屏幕显示 Wi-Fi QR、名称、密码、URL。
3. 手机在设备 Web 页面扫描/输入 Wi-Fi，填写资源服务地址，提交后可离开页面。
4. 后台先下载字体，再下载商城；主线程持续驱动 LVGL 与触摸。
5. 显示当前阶段、文件、完成大小、百分比、用时和错误/重试入口。
6. SHA-256 校验成功后才激活文件。商城调用现有事务安装器，写入目录服务地址。
7. 完成后退出引导、释放临时线程栈与缓冲、关闭热点，进入 Launcher。

SD 上的 `aster.setup.pending` KV 标记在修改资源前写入，字体、商城和服务器配置全部成功后
才移除。它保证安装中断后不会因文件已存在而跳过最后的配置。已健康运行的旧卡没有该标记，
继续离线启动。已经连接过网络的卡会等待平台的异步重连，避免把连接中的 EBUSY 当成缺少配置。
网页请求有超时，表单错误不会被状态轮询覆盖；成功状态会保留，热点延迟关闭以便手机读到结果。

没有 SD 时显示插卡提示，不自动格式化。下载与已有应用/存档目录隔离。
启动检查读取字体头与末记录/文件边界，不在每次启动完整扫描约 92 MiB；下载时做完整摘要校验。
这不是通用固件 OTA，当前只初始化完整字库与商城依赖。

Zephyr 可通过 `CONFIG_ASTER_FIRST_BOOT=n` 禁用以供无网络的特殊构建；正常 lckfb
配置默认开启。P4 UI 和 runtime 直接编入同一组公共源码。共享服务不会调用 LVGL；
配网线程也不写安装目录；UI 只读取带锁快照，安装在资源线程执行。

## 构建和验证

服务端启动与发布说明在 SDK 的 `docs/setup-server.md`。
P4 使用现有 `tools/bringup.py osptek-p4 build`，lckfb 使用 `tools/firmware.py build`。

无板测试：

```sh
cc -std=c11 -Wall -Wextra -Werror -I common/setup tests/setup_core_host.c common/setup/aster_setup_core.c -o build/host/setup-core
build/host/setup-core
cc -std=c11 -Wall -Wextra -Werror -I common/setup tests/setup_json_host.c common/setup/aster_setup_json.c -o build/host/setup-json
build/host/setup-json
python -m unittest discover -s tests -p test_setup_http.py
python -m unittest discover -s tests -p test_setup_stream.py
python -m unittest discover -s tests -p test_font_cache.py
node tests/setup_page_host.mjs
cmake -S tests/setup_preview -B build/setup-preview -G Ninja
cmake --build build/setup-preview
build/setup-preview/setup-preview 480 480 build/setup-preview
build/setup-preview/setup-preview 240 320 build/setup-preview
```

预览链接真实 LVGL、系统字体和共享引导源码，生成 PPM 并检查文字边界；不模拟无线电、SD 或安装。
实机验收必须覆盖空卡完整初始化、断网重试、断电续传、健康卡离线启动，以及两块板
AP/STA 并发。在完成这些之前，编译和主机测试不能作为首次开机全流程实机验证。

开发期间可先停止运行的 APP，再用 P4 `aster ui setup` / Zephyr `aster sys setup`
重新进入引导，保留卡内数据。已有资源会校验后跳过，因此这不等价于空卡测试。

需要清空卡做首次开机测试时，原生 CLI 提供 `aster sd format --confirm`。
该命令会删除 SD 卡全部内容；先备份存档、配置及无法重新获取的文件。
仅接受已经挂载的卡，且必须停止所有 APP、结束配网/安装后执行；缺少确认参数不会格式化。
UI 线程先隐藏桌面并关闭字体缓存文件，再由存储适配器格式化并重启。
P4 使用 IDF 的 FAT 格式化接口，lckfb 使用 FatFS FAT32；两者均不在挂载失败时自动格式化。
这不是修复无法挂载或损坏卡的工具，也不会清除芯片 Flash 中的 Wi-Fi 配置。

2026-11-09 实机记录：

- P4、lckfb 均已编译、烧录并通过健康卡启动检查。
- P4 已实际执行设备端 `aster sd format --confirm`：缺少确认参数和 APP 运行时均拒绝；
  正常执行约 20 秒后自动重启，APP 列表为空并进入欢迎页。
  操作前经设备读回并校验了 APP data、系统 KV 和音乐目录，共 17 文件、8376 字节；
  本地备份位于工作区 `backups/p4-user-data-20261109`，程序、字体和资源由服务器重新获取。
- P4 格式化后已通过手机配网完成空卡初始化：服务端确认全部 11 项载荷均有实际请求，
  设备报告 phase=9、error=0、96378488/96378488 字节，热点关闭；
  APP 列表仅有已启动的商城 0.1.8。用户确认完成，并要求不恢复旧备份。
- lckfb 已烧录格式化功能并实际执行 `aster sd format --confirm`：
  缺少确认参数及商城运行时不执行格式化，停止 APP 后约 40 秒完成并重启。
  重启确认 `no apps installed`、字体目录为空，进入手机配网阶段；
  随后已完成手机配网、全部 11 项载荷实际下载与校验，进入 phase=9，
  APP 列表确认商城 0.1.8 已安装；此轮全量资源准备约 10 分钟。
- 两块板的 AP/STA 并发与设备 Web 服务已运行；浏览器表单在 lckfb 上实际提交并显示进度。
- 早期保留数据测试中，P4 完成资源校验、商城下载安装和返回 Launcher；字体摘要一致，跳过已有字体下载。
- 早期保留数据测试中，lckfb 备份原字体目录后，从缺字体状态下载十个字号并安装商城 0.1.8，返回 Launcher。
- lckfb 下载过程中停止本地服务器，保留了 856,752 字节的临时文件；重启后实际收到 HTTP 206，
  续传后通过完整 SHA-256 校验。该早期字体备份后来随空卡测试的格式化一并清除。
- 修复 lckfb AP 客户端加入时 DHCP 回调所在 socket service 线程的栈不足：2400 → 4096 字节。
- 修复私有 HTTP 服务的瞬时 EAGAIN，以及状态/关闭/区间头读取的重试处理。
- 两块板均已验证格式化后空 SD 卡初始化；芯片 Flash 的既有配置没有恢复出厂。
  健康卡完全离线启动、无卡热插拔、写保护卡和公网 HTTPS 尚未做完整实机验收。

下载诊断日志将单文件接收/写入与后续读回校验分开：`setup transfer` 的
`wall_ms` 为正文接收及写入耗时，`sd_write_ms` 包含写入、同步和关闭，
`wait_ms` 是接收队列暂时为空或忙时的累计让出时间；网络收包与写卡可重叠，
不能把该等待值当成纯 Wi-Fi 延迟。`setup hash` 分别记录 SD 读取与 SHA256 调用耗时。
首次开机复用现有临时 16 KiB 缓存合并小块写入，完成或出错会释放资源线程上下文；
没有增加常驻 SRAM 或扩大公共 HTTP/TCP 队列。接收等待改为 1 ms，让低优先级 TCP 线程运行。

2026-11-09 lckfb 同卡、同服务器、同一 `ui_12.cpfont`（2,283,159 字节）对比：

- 原来正文接收并写卡 12,339 ms，其中写卡 10,433 ms；读回校验 3,214 ms。
- 16 KiB 合并写入后 8,551 ms，其中写卡 2,988 ms、等待 5,220 ms。
- 再将接收等待缩短至 1 ms 后，接收并写卡 6,534 ms，其中写卡 3,484 ms、等待 2,585 ms；
  读回校验 3,211 ms。正文阶段约快 1.89 倍，含校验约快 1.60 倍。

这是单文件实测，不是优化后整套约 92 MiB 资源的耗时承诺。lckfb 当前 SD 总线配置为
1 位、10 MHz；原测该文件读回约 1.08 MB/s。保留完整读回 SHA256，不通过省略校验提速。
此改动仅优化共享首次开机下载器；商城 APP 自己的资源下载循环并未因此自动改成合并写入。
最终版已烧录 lckfb，复测字体及商城载荷下载、全部资源校验和商城安装均通过，返回 phase=9；
测速临时副本已删除。P4 使用同一份下载器源码，已编译通过，尚未烧录此速度调整。
