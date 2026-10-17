# 网络功能实施交接：Cursor 从 N5.1 继续

交接日期：2026-10-17。本文件随固件源码提交；状态以本文件、实施方案和验证记录共同为准。当前提交是阶段性检查点，不是 N5.1 完成版本。不要仅因已提交就将阶段标题标为“已完成”。

## 1. 可以直接交给 Cursor 的任务指示

> 请读取本文件、`docs/network-services-implementation.zh-CN.md`、`docs/network-websocket-cli.zh-CN.md` 和 `docs/validation/network-n5-ws-cli-2026-10-17.md`，接着实施网络计划。先核实工作区及开发板型号，补齐 LCKFB 的 N5.1 WebSocket/WSS CLI、异常恢复、资源和图形并发验收；随后封装 SDK/APP，完成 Windows/Linux 模拟器和双板 APP 验收。审查现有代码，有问题可以修改，但需要保留失败证据、补做受影响的回归。遵守实施方案 10.0 节 G1/G2 门槛，不能把构建或 CLI 成功当成完整交付。当前公共 ABI 为 3.11，尚无 WS APP 导入。按 N5 子项依次推进，N6 才统一网络 ABI 并移除旧网络兼容层。硬件需要切换、管理员操作或实体屏确认时明确通知我；反馈之前保持被验收 APP 运行。只做本地实施，不擅自推送、发布 Release、改 tag 或提交测试日志/图片/证书。

用户允许修改和完善现有代码，不要求保留有问题的实现。不要重新从 N1 开始。当前网络实施文档才是范围来源，不依赖早期临时优化路线图。

## 2. 仓库、已完成工作及当前边界

工作区：`/home/myj/zephyrproject/aster`。根目录不是 Git 仓库，三个仓库分别为：

- `aster-firmware`：Zephyr 固件、公共契约、网络后台服务和设备 CLI。
- `aster-sdk`：独立 APP 开发工具、契约快照、示例和发布工具。
- `aster-sim`：Windows/Linux 模拟器。

交接前均在 `main`。N4 基线本地提交分别为固件 `8a71fd2`、SDK `cbbd5ec`、模拟器 `5e34c85`。本次仅提交固件 N5.1 阶段增量和交接文档，SDK、模拟器没有 N5 增量。准确交接提交使用 `git log -1` 查看。本次未推送或发布。

N1 TCP/UDP、N2 HTTP/HTTPS、N3 MQTT/MQTTS、N4 复合验收已完成既定阶段工作。公共 ABI 当前 `3.11.0`。流式音频已有实现，不能改坏在线音乐及其网络、音频资源回收。

N5.1 当前做到：

- 内部有界 WS/WSS 编解码、后台服务、设备 CLI、主机测试及局域网测试工具已实现。
- 固件主机 122 项检查通过，包含真实 C 服务/编解码 ASan/UBSan；主机 TLS 桩不等同真实 TLS 验证。
- 微雪 3.5B 已完成 15 类局域网 CLI 检查、断网恢复、WSS/HTTP 图形并发及用户实体交互确认。
- **LCKFB 仅构建了 N5.1 诊断固件，尚未刷入或验收。** 用户尚未确认切换到 LCKFB；看到串口不等于板型正确。
- **尚未实现 WS 的公开契约、native/WAMR 桥接、SDK、示例 APP、模拟器，也未做双板 APP G2。**
- 默认 `CONFIG_ASTER_WEBSOCKET=n`；不能声称当前发布 SDK 已支持 WS。
- N5.2–N5.5、N6 尚未开始；完整范围读实施方案，不自行增加协议或常驻服务。

重要既有修复：N4 在异步画布写入前等待缓冲可写，以及 LVGL 指针事件 FIFO 保留短按下/抬起。不要回退，否则复合网络场景可能再次闪烁或触摸失效。

## 3. 现有实现入口和资源约束

主要文件：

- `systems/zephyr/services/aster_ws_codec.{h,c}`：RFC6455 有界协议解析、mask、UTF-8、握手辅助。
- `systems/zephyr/services/aster_ws.{h,c}`：owner、句柄、状态、缓存、超时、TLS/传输配额及退出回收。
- `systems/zephyr/services/aster_http.c`：在现有 worker 中条件调用 WS tick，不新建线程。
- `systems/zephyr/shell/aster_ws_cmd.{h,c}`、`aster_net_cmd.c`：内部服务 CLI。
- `Kconfig`、相关 `CMakeLists.txt`、`config/diagnostics/network-websocket.conf`：默认关闭、诊断开启。
- `tests/test_ws.py`、`ws_codec_host.c`、`ws_service_host.c`：真实 C 主机测试。
- `tools/validation/network_ws_peer.cjs`：局域网服务端，Windows 原生 Node。
- `tools/validation/network_ws_capture.py`：CLI 基础/异常/TLS/配额/内容检查。
- `tools/validation/network_ws_concurrent.py`：断网恢复及前台 APP + WSS + HTTP 检查。
- `tools/validation/network_http_capture.py`：共用串口命令/HTTP 内容校验；已增加 WS 响应识别。

固定预算：全局一个 WS 客户端，和 HTTP、MQTT、TCP/UDP、CLI 共用两个传输名额；WSS 与 HTTPS/MQTTS 共用一个 TLS 名额、现有 TLS 池及信任源。不新增私有 TLS 池、网络栈或常驻线程。

单条文本/二进制消息最多 4096 B；保留消息边界、合法空消息、短缓冲所需长度。接收支持分片及穿插控制帧；发送入队成功不表示服务端业务确认。nonce 和每帧 mask 使用系统密码学随机源，失败必须报错。禁止关闭证书验证或固定 mask。

内部状态和 errno 仅用于固件 CLI，不可直接变成公共 ABI。当前 close 立即使句柄失效，后台关闭宽限最多 1500 ms；cancel 保留终态句柄，之后仍需 close。CLI 有界 EAGAIN 重试是正常的锁竞争处理，不得把已接受的业务操作盲目重发。

微雪已观察到：明文 WS 私有 PSRAM 18040 B；WSS 19887 B（含测试 CA 和 TLS 上下文），另有底层 TCP 槽 4096 B。WSS+HTTP 样本为 WS 19887 B、HTTP 23312 B、两个 TCP 槽共 8192 B；TLS 内部约 33536 B 使用原有静态 96 KiB 保留，不能重复算为新增 PSRAM。HTTP 栈 8192 B、网络栈 4096 B，无 WS 新线程。精确采样及限制见验证记录，不将统计命令间的样本当原子峰值。

后续代码审查关注：超时是否覆盖所有等待阶段、慢速逐字节发送是否可无限延长单帧接收、背压时控制帧与关闭的进展、owner 代次隔离、并发取消/退出及 TLS CA 引用释放。当前不承诺这些边界已经穷尽，需依据 G1/G2 补测试，不靠改文档消除问题。

## 4. 实机、PC 网络和测试环境

交接前最后核实的微雪状态：

- `/dev/ttyACM0`，设备标识 `aster.esp32s3.waveshare35b`。
- 已恢复日常固件、主页；原 14 个 APP 都 STOPPED，音量 53，Wi-Fi 保留。
- 日常镜像 SHA256：`6291edaffaca107e8baae63ee6b185ac56211363a6695ddbb350cd8401d94057`。
- 日常配置路径 `build/qspi-prefetch40`，RGB565、40 MHz、双 8 KiB 内部缓冲、96 KiB 内核堆。不得恢复此前失败的 RGB666/80 MHz 实验。
- 本轮上传 CA 已删除，默认信任源恢复；测试连接及 TLS 用量归零；未安装 N5 临时 APP。

最后观测 PC 为 Windows Wi-Fi `192.168.110.201`、Private 网络；微雪 `192.168.110.92`，二者同局域网可互通。**这些地址和 SSID 不是固定配置，重新测试必须重新确认。** PC 挂 VPN 不代表开发板走 VPN；用 Windows 原生 Node 绑定局域网地址，不能把 WSL NAT 地址直接当设备可达地址。

LCKFB 通常使用 `/dev/ttyUSB0`，可能同时有原生 USB ACM。上次 N4 留存为 17 APP、音量 74；本轮应重新采集实际基线，不能照抄。确保构建配置 `CONFIG_ASTER_HW_LCKFB_S3=y` 且微雪选项关闭，避免混板配置。

测试需要独占串口：烧录、SDK 文件传输、测试 CLI、串口监视器不可同时抢占。不要复用历史会话编号；没有活跃测试时释放串口。主机工具位于 `/home/myj/zephyrproject/.venv/bin`，Windows Node 最后可用路径 `/mnt/d/Program Files/nodejs/node.exe`，接手时核实存在。

原始证据都在仓库外 `../diagnostics/network-n5-20261017/`：

- `firmware-tests-final.txt`：此前 122 项通过；`firmware-tests-handoff.txt`：交接前重跑 122 项通过，耗时 19.366 秒。
- `wave-cli-r3/result.json`、`serial.txt`：15 类 CLI 通过。
- `wave-ui-r1*`：断网恢复和实体反馈通过，但自动 HTTP 收尾超时失败，**不能标成整轮通过**。
- `wave-ui-r2*`：修正测试脚本后，通过 6 轮 WSS 回环和一份完整 16 KiB HTTP 内容校验；属于有界串口校验，不是带宽测试。
- `restore-wave.txt`、`flash-restore-wave.txt`：日常恢复。恢复后探测 `aster net ws stats` 会因功能默认关闭而没有 WS 前缀，旧采集器等待后超时；这不代表运行中的 WS 泄漏。使用镜像校验、配置与普通系统/网络命令核实。
- `run_peer.py`、`certs/`：本机测试启动器及私有证书，不进 Git。不把日志、私钥、SSID 密码或镜像放进交接提交。

第一轮脚本曾未重试 WSS 状态/关闭的 EAGAIN，遗留测试句柄；已回收、修正并完整重跑。并发第一轮将 16 KiB HTTP 留到人工反馈之后才读，超过 600 秒总期限返回 -116；已改为循环中读取，验证完整内容后再发下一请求。失败原始记录保留，不能删除或当固件缺陷的已证实原因。新长时用例应避免再次让“等待人工反馈”消耗网络请求总期限。

交接时已停止本轮 Windows Node 对端（PID 42876）。尝试删除临时防火墙规则被 Windows 拒绝访问，**规则尚待用户在管理员 PowerShell 删除**。清理记录为 `../diagnostics/network-n5-20261017/handoff-host-cleanup.txt`；不要将防火墙写成已清理。

## 5. 下一步：补齐 LCKFB 的 G1

先查看各仓库 `git status` 和当前设备、网络；先读脚本 `--help`，确认调用与本文件一致。下面命令从 `aster-firmware` 执行，日志保存在仓库外；使用新输出目录保留旧证据。

### 主机测试与构建

```bash
/home/myj/zephyrproject/.venv/bin/python -m unittest discover -s tests
```

既有 LCKFB 诊断目录是 `build/network-n5-lckfb`。可检查其 CMakeCache、`.config` 后增量构建；重建示例：

```bash
PATH=/home/myj/zephyrproject/.venv/bin:$PATH west build \
  -b esp32s3_devkitc/esp32s3/procpu -d build/network-n5-lckfb -- \
  -DASTER_USER_CONFIG=/home/myj/zephyrproject/aster/diagnostics/graphics-g5-lckfb-20261014/production.conf \
  '-DEXTRA_CONF_FILE=config/diagnostics/network-http.conf;config/diagnostics/network-pools.conf;config/diagnostics/network-websocket.conf'
```

该 `production.conf` 是本机历史基线，不保证其他机器存在。缺失时先按仓库板级文档确认，用仓库外文件设置以下既有 LCKFB 配置，不能误用微雪片段：

```text
CONFIG_ASTER_SYSTEM_ZEPHYR=y
CONFIG_ASTER_HW_LCKFB_S3=y
CONFIG_ASTER_BOOT_PORTRAIT=y
CONFIG_ASTER_CFG_CAMERA=y
CONFIG_ASTER_CFG_AUDIO=y
CONFIG_ASTER_CFG_WIFI=y
CONFIG_ASTER_CFG_LAUNCHER=y
CONFIG_ASTER_DISP_TIMING=n
CONFIG_ASTER_GFX_TIMING=n
```

烧录前核实设备型号、SD 和原 APP/音量/网络，保存当前日常镜像及 hash：

```bash
PATH=/home/myj/zephyrproject/.venv/bin:$PATH west flash \
  --skip-rebuild -d build/network-n5-lckfb --esp-device /dev/ttyUSB0
```

### 局域网对端及证书

服务端 `network_ws_peer.cjs` 需要安装好的 Node `ws` 模块，可复用 `../aster-sim/node_modules/ws`，不向固件产品增加依赖。私有 `certs/` 需要 `ca.pem`、`server.pem/key`、`wrong.pem/key`、`expired.pem/key`；valid 证书 SAN 必须覆盖**当前 PC 局域网 IP**，wrong 必须故意名称不符，expired 必须实际过期，三者须由测试 CA 正确签发。不要因测试困难跳过证书验证。跨日重测核对证书日期和设备时钟。

优先复用本机 `../diagnostics/network-n5-20261017/run_peer.py`，确认 IP、证书有效期与模块路径后执行。它通过 `wslpath -w` 将资源路径传给 Windows 原生 Node；工作区换机器时自行重建启动器，不把私钥写到仓库。端口为：17880 明文 WS/HTTP，17881 正常 WSS/HTTPS，17882 错名称，17883 过期，17884/17885 停滞对照。停止服务只终止命令行属于该 fixture 的进程，不杀全部 Node。

需要管理员放行时，通知用户在管理员 PowerShell 执行下面**一条**命令，等待明确反馈并核实；不要把多个命令不加分隔符粘在一起：

```powershell
New-NetFirewallRule -DisplayName 'Aster N5.1 local validation' -Direction Inbound -Action Allow -Protocol TCP -LocalPort '17880-17885' -RemoteAddress LocalSubnet -Profile Private
```

### CLI 与图形并发命令

下面 IP、串口、输出路径先按现场修改。CA 由工具上传，不在聊天中要求用户提供 Wi-Fi 密码；用户可通过设备 UI 联网。

```bash
/home/myj/zephyrproject/.venv/bin/python tools/validation/network_ws_capture.py \
  --port /dev/ttyUSB0 --host 192.168.110.201 \
  --sdk-tools ../aster-sdk/tools \
  --ca ../diagnostics/network-n5-20261017/certs/ca.pem \
  --output ../diagnostics/network-n5-20261017/lckfb-cli-r1

/home/myj/zephyrproject/.venv/bin/python tools/validation/network_ws_concurrent.py \
  --port /dev/ttyUSB0 --host 192.168.110.201 \
  --sdk-tools ../aster-sdk/tools \
  --ca ../diagnostics/network-n5-20261017/certs/ca.pem \
  --output ../diagnostics/network-n5-20261017/lckfb-ui-r1 \
  --app aster.game.2048 --seconds 600 --wifi-recovery \
  --stop-file ../diagnostics/network-n5-20261017/lckfb-ui-r1.stop
```

`--stop-file` 必须开始时不存在；创建它只结束网络循环并清理，脚本仍保持前台 APP。超过 seconds 会自动结束网络，因此不能在进程已经退出后声称“仍有后台 WSS”。不要用旧停止文件让循环立即结束。

收集用户实体反馈之前：确认 APP 状态 RUNNING、页面实际显示，WSS 已 READY 且真实消息校验成功，HTTP 确实占第二槽；然后请用户滑动游戏、上拉取消、下拉再收回。保持 APP 和网络运行直到反馈；若超时退出，重新建立所需负载后再验收。测试期间意外断线需询问是否人为拔 USB，不能直接定为固件崩溃。

现有脚本不是 G1 全覆盖证明。对照 10.0 逐项补 DNS 失败、不可达端口、各等待阶段超时/取消、慢帧、慢消费、控制帧、连接耗尽、关闭后再开、连续生命周期；协议/服务主机测试和实机证据分别注明。微雪遗漏或受修复影响的项目也补测，不照抄 LCKFB 结果。

成功条件：不仅 exit=0，还要内容正确、证书错误拒绝、恢复可重新连接、队列有限、CLI/UI 响应，最终 WS/HTTP/NET active/closing/PSRAM 和 TLS current 回基线。分别记内核堆、libc、PSRAM、线程栈，说明首次初始化/常驻栈及非原子快照，不能只报一个“内存用了多少”。

## 6. G1 后封装 SDK、APP 与模拟器

1. 评估并固定公开结构/异步状态/错误域、文本与二进制边界、空消息与无消息区别、容量不足所需长度、取消与关闭语义。参考当前内部服务，不照搬私有 errno、宿主指针、`size_t` 或 fd。ABI 版本需按现有规则决定；不要把尚未选择的 3.12 写成已交付。
2. 以固件 `contract/include/aster` 为契约源，通过 `tools/export_contract.py`、SDK `tools/import_contract.py` 的现有流程同步。先读工具帮助/既有 N3 用法，避免手工双写漂移；更新 API 清单、版本、能力、权限及 ABI 冻结检查。
3. 在 native/WAMR 桥接中校验 `net` 权限、能力、当前 APP owner、WASM 地址和长度、整数溢出、结构大小/版本/保留字段；后台服务不持有 WASM 指针、不回调 APP 或 LVGL。APP 停止、初始化失败、trap 都回收其后台资源。
4. SDK 添加可直接构建的远程控制/回环示例、文档及测试，APP 只能调用 SDK API，不能发 shell 字符串。必须能独立于固件源码开发。
5. 模拟器按同名公开接口实现权限、状态、消息边界、配额、TLS验证、取消和 owner 回收；真实联网需显式启用，确定性回放仍可离线。先看 `aster-sim/test/{network,http,mqtt}.test.js` 及已有真实包集成方式，再扩展 WS，不能只验证 mock。
6. 两板部署真实 WASM/AOT 包完成 G2：成功收发及内容、4096/4097 与空消息、短结构/短缓冲/越界/溢出、无权限、过期/他人/CLI 句柄、两传输/一TLS争用、断线及取消、退出/初始化失败/trap 回收；至少按原方案完成生命周期次数，100轮主机 fixture 不替代两板 APP 测试。
7. 两板分别在真实 APP 后台 WSS+另一协议的负载下验证画面/触摸/上下拉恢复，收集实体反馈。Windows/Linux 构建和所需测试零跳过，不把一平台成功写成双平台成功。
8. 更新 SDK 手册/技能、示例、模拟器说明、契约与配套版本关系，避免仍写旧 ABI 或硬件 target。未正式发布不能声称用户从旧 Release 可下载新增接口；不主动生成/上传新 Release。

N6 用户已明确不保留旧网络 ABI 兼容层，但**在 N6 实施时统一切换**；现在不提前删除 N1–N5 已验证导入，也不顺便重构图形/音频 ABI。N5.1 完整验收后再按主实施文档推进 N5.2 等子项。

## 7. 每轮结束的清理和完成定义

- 取消并关闭测试句柄，等后台 closing=0，恢复默认信任源，删除 SD 临时 CA、测试 APP 和上传物；保留用户原 APP、存档、音量、网络。
- 恢复所测板日常固件，核对纯板型、诊断/故障注入关闭和版本；检查原 APP、主页或约定原前台。别把诊断 build 当正式恢复镜像。
- 微雪日常目录 `build/qspi-prefetch40`；LCKFB 既有日常目录 `build/gfx-production-lckfb`，原已知镜像 SHA256 `aa6381107596340fc0ea58d8f6aa3f2437ef8650a4ba51edfad48adbb7afb4f1`。恢复前保存/核对 hash。为防 `west flash` 自动重建覆盖冻结镜像，使用 `--skip-rebuild`；如果源码变化需要新日常固件，另建目录并明确回归范围。
- 停止本轮 fixture、释放串口，管理员 PowerShell 删除临时规则并核实：

```powershell
Remove-NetFirewallRule -DisplayName 'Aster N5.1 local validation'
```

- 将结果写入源码 `docs/validation/`，记录配置、代码版本、通过/失败、资源与限制、用户实体反馈、清理状态。日志、图片、证书、私钥、固件/AOT包只留仓库外 diagnostics，不能 `git add .` 混入。
- 各项标题/勾选仅在对应代码、测试和约定验收真正完成后更新。G1 过了但 G2 未过仍是实施中；不要将 N5.1 完成等同整个 N5 完成。
- 如果缺硬件、管理员权限或用户实体确认，清楚记录未完成项并继续不依赖它的工作，不编造通过；只在必要时请求具体协助。

用户后续要求提交时，用中文提交说明，按仓库和范围逐项检查暂存内容；未获要求不推送、不合并、不创建 Release。交接后第一步是确认实际接入的是哪块板，而不是再问已经通过的微雪实体问题。
