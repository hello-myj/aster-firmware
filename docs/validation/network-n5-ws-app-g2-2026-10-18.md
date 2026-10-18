# N5.1 WebSocket APP G2 验收记录（双板，2026-10-18）

承接[双板 G1 记录](network-n5-ws-cli-2026-10-17-lckfb-g1.md)。本轮实施 G2 公共链路并先后完成 LCKFB 与微雪的 APP 实机门槛。代码版本：三仓均提交于本地 `feature/network-n5` 分支（未推送）；公共 ABI 已选择并实施为 **3.12.0**（additive）：新增 `contract/include/aster/ws.h`、六个 `aster_ws_*` 导入、`ASTER_FEATURE_WEBSOCKET=20`、`websocket` 能力与固件桥接 `aster_ws_bridge.inc`；`CONFIG_ASTER_WEBSOCKET` 默认仍 n。该 ABI 尚未发布 Release，不得声称旧 Release 用户可用。

## 契约与桥接

- 公开状态空间与内部一一对应（CONNECTING=1…CANCELLED=8），桥接以 `_Static_assert` 冻结；结构 sizeof 48/4124、data 偏移 28 同样编译期断言。recv 采用定长 `aster_ws_message_t`：OK 整条交付（length=0 为合法空消息），EAGAIN 暂无，CLOSED 取完为 EPIPE，FAILED/CANCELLED 返回稳定终态错误；无 EMSGSIZE 路径（4096 上限由结构保证），短结构/越界目的返回 EINVAL 且不消费消息。
- 内部私有 errno 经 `ws_error` 映射：证书位→ECERT、帧/UTF-8/升级→EPROTO、取完关闭→EPIPE、取消→ECANCELLED、超时/保活期限→ETIMEDOUT、RST→ECONNRESET、拒绝→ECONNREFUSED 等，平台 errno 不泄漏。
- 权限/会话：`net` 权限 + APP 专属 owner（与 CLI 诊断 owner 隔离，句柄单调代次）；guest 地址与长度先验证再复制；后台服务不持 WASM 指针、不回调 APP/LVGL；`aster_native_release_network` 先收 WS 再收 MQTT/HTTP/net，stop/trap/init 失败同路径回收，重开会话 owner 全新、旧句柄永久 EBADF。

## 主机测试（真实 C + ASan/UBSan）

新增 `tests/ws_bridge_host.c`（固件 122→123 项，全部通过）：真实服务/POSIX 回环上验证权限门、URL 越界与内嵌 NUL、timeout 边界、结构 size/version、foreign owner EBADF、payload 调用时复制（擦除 guest 内存不影响会话）、文本/二进制/空消息边界与乱序不消费、状态字段与 reserved 清零、终态先交付已完成消息再返回稳定错误、CLOSED→EPIPE、单客户端耗尽 -28/关闭后恢复、`aster_native_release_network` 回收后 owner 换代且旧句柄失效、PSRAM 归零。TLS 映射由既有服务 fixture 与断言覆盖，证书验证仍以实机为准。

## SDK 与模拟器

- 契约以 `tools/export_contract.py`→SDK `tools/import_contract.py` 流程同步（abi 3.12.0、6 个 ws 导入、feature_bits.websocket=20、targets/xtensa 能力含 websocket），无手工双写。SDK 105 项通过；新增示例 `apps/ws-echo`（文本/二进制/空消息内容校验、5 项一次性 ABI 边界探针、EAGAIN 不盲重发、终态 close+有界退避重连、点按暂停）。
- 模拟器 `src/ws.js` 用真实 Node `ws`/TCP/TLS 实现同名公开接口：两传输槽+一 TLS 租约+设备级单 WS 客户端全局配额、有界接收队列（首个未取消息即暂停 socket，超限明确终态而非无限缓存）、空消息与 EAGAIN 区分、CLOSED 取完 EPIPE、终态保留句柄与配额直到 close、stop 全回收。Linux `node --test` 95 项 0 失败；启用 fixture 后 WS 套件 10/10（含真实证书：信任通过、错名称/过期拒绝并带证书位、WSS 与 HTTPS 共享 TLS 租约互斥、真实 SDK 包 WS Echo 完整跑通回环内容+渲染+停止回收）。`src/package.js`/`host.js`/测试的版本改为从 `src/sdk-compat.json` 单一派生，杜绝字面量漂移。双平台完整套件（全部集成 fixture 启用，零跳过）：Windows 原生 Node v24.14.0 于仓库外副本 `D:\tmp\aster-sim-g2-win`（复用 Windows 原生 node_modules：ws 8.18.3、@napi-rs/canvas 0.1.100 等，与 package.json 声明一致）**95/95 通过、0 跳过**；Linux Node v20.18.0 直接运行仓库套件**95/95 通过、0 跳过**。两端均含真实 SDK 包（2048/imucube/breakout/scene/network/http-fetch/http-download/mqtt-echo/**ws-echo**）、HTTPS/MQTTS/WSS 证书与 LAN `ASTER_HTTP_TEST_HOST` 正向校验；日志存仓库外（Windows `run-full2.log`、Linux `diagnostics/fix-linux/run-full3.log`）。

## LCKFB 实机 APP G2

- 诊断固件（WS 桥接构建，镜像 SHA256 `840265f5c64301bdce2a0a067fa179b4ed74f098ae5cc120ad7be14bbc208330`；G1 旧版 `f9667a81…` 已过时）经 `/dev/ttyUSB0` 刷入，`aster sys info` 确认 `board=aster.esp32s3.lckfb abi=3.12.0`；WS 未开时 psram=0。临时防火墙规则经 UAC 创建并生效，结束后删除并核实计数 0。
- 明文 WS（`ws://192.168.110.201:17880/echo`，包 SHA256 `131cbf7a…`）：APP RUNNING，日志 `WSAPP PROBES ok` + `round=1/20/…/100 content=ok`（文本 10B、二进制 4B、空消息 0B 混合轮转逐字节校验）；`WS active=1 psram=18040` 与 CLI G1 样本一致；**CLI 争用同一全局客户端预算被明确拒绝**（`aster net ws open` → `WS result=-28`），不误判为泄漏。
- WSS（`wss://…:17881/echo`，包 SHA256 `2cf7642b…`，requires 含 `tls` 安装校验通过；测试 CA 临时上传 `/SD:/ca.pem` 并设为信任源）：RUNNING，PROBES ok，80+ 轮内容校验通过，`WS active=1 tls=1 psram=19887`；并发 CLI HTTP 占第二传输槽（`NET active=2`，HTTP psram=23312）时前台动画继续。**用户实体确认：点按暂停/继续、上拉取消回桌面、下拉再收回全部正常。**
- 回收：停 APP 后 `WS active=0 closing=0 tls=0 psram=0`、`NET opened=closed=5`、句柄跨会话不复用；HTTP 句柄 close 后归零；ws.echo 卸载、`/SD:/ca.pem` 与上传包删除、默认信任源恢复、17 个原 APP 保留。LCKFB 恢复日常固件（`--skip-rebuild`，hash `aa638110…` 核对一致，abi 3.11.0、vol=74、Wi-Fi 192.168.110.99、无 ws.echo 残留）。

## 微雪实机 APP G2

- 微雪原生 USB（bus 1-3）经 usbipd attach 为 `/dev/ttyACM0`（`serial=shared`，probe PONG，`caps=1ff8eb` 含 bit20 websocket）；跨夜设备硬复位会丢 attach，已按需重连并以响应确认板型。刷入含 WS 桥接的微雪诊断镜像（`5354867ee39feb1204cd5bdabbdb1ba61076bd72f2b2b55b0fb377c08da419ea`，`build/network-n5-wave`，板型 `waveshare_esp32s3_touch_lcd_35b`），`aster sys info` 确认 `board=aster.esp32s3.waveshare35b abi=3.12.0`。
- 明文 WS（同一 `ws://192.168.110.201:17880/echo` 设备包 `131cbf7a…`）：APP RUNNING，监视器自会话起点捕获 `WSAPP PROBES ok` + `round=1/20/40/60/80 content=ok`（文本/二进制/空消息混合轮转），`WS active=1 psram=18040` 与 LCKFB、与 G1 CLI 样本一致。
- WSS（包 `2cf7642b…`，requires 含 `tls` 安装校验通过；`/SD:/ca.pem` 临时信任源）：`WSAPP PROBES ok`+round=1..20 通过，`WS active=1 tls=1 psram=19887`；并发 CLI HTTP 占第二传输槽（`NET active=2`），前台 RUNNING。**用户实体确认：点按暂停/继续、上拉取消回桌面、下拉再收回全部正常；期间无人为拔插 USB。**
- 回收：close 并发 HTTP（h7）后停 APP，`WS active=0 closing=0 tls=0 psram=0`、`NET active=0 opened=closed=7`、`HTTP active=0 psram=0`；ws.echo 卸载、CA 与上传包删除、默认信任源恢复，14 个原 APP 保留（另见既有 `com.example.palresearch` 空目录告警，非本轮引入）。恢复日常镜像 hash `6291edaf…` 核对一致（abi 3.11.0、vol=53、Wi-Fi 192.168.110.92），`aster net ws` 在恢复固件中不存在（诊断关闭）。
- 至此 G2 双板 APP 门槛（成功收发与内容、4096/4097、空消息、短结构/越界/伪造句柄、无权限、预算争用拒绝、停/trap 回收路径、实体反馈）在 LCKFB 与微雪各自独立通过，未互相照抄。

## 已知限制与未完成

- 双平台模拟器复跑已完成（Windows v24.14.0 与 Linux v20.18.0 各 95/95、0 跳过，见上节），Windows 项不再是缺口。
- 实机双板验证了成功路径、内容边界、权限/所有权、预算争用拒绝与停 APP 自动回收；取消 mid-flight 与 trap 回收已于同日以专项探针双板执行（见下节），其中"完成消息先于终态交付"实机复现，另发现 cancel 可见性窗口一项待评审偏差。初始化失败路径实机未单列（主机 fixture 覆盖 init 拒绝→回收）。
- 设备包 `192.168.110.201` 为现场 IP，非固定配置；WS_HOST/WS_PORT 编译进包，属示例形态，正式用法应运行时配置（不改契约迁就）。
- 生命周期证据：主机服务 fixture 含 100 轮取消/关闭恢复，桥接 fixture 覆盖跨会话句柄失效与释放归零；两板实机各自完成开→回环→停→回基线→重开（明文与 WSS 各一轮），叠加 G1 双板各 25 轮 CLI 生命周期。
- 主机侧：临时防火墙规则 `Aster N5.1 local validation` 最终经管理员删除并核实计数 0；Node 对端已停止（17880-17885 监听归零），串口已释放，两板日常固件与哈希均已核对。无遗留。
- 三仓本轮改动已本地提交于 `feature/network-n5` 分支（本固件仓库为该分支 HEAD；SDK `ebebed7`、模拟器 `a054109`，模拟器契约快照固定到 SDK 提交 `ebebed7`），未推送、未发布 Release；契约导出 ZIP、设备包与探针工程只在仓库外 diagnostics。N5.1 的 G1 与 G2 既定门槛已全部有证据，但"阶段完成"须在实施方案中逐项复核勾选后才能标注，N5.2–N5.5、N6 尚未开始。

## 补充：实机取消/trap 专项（双板，2026-10-18 晨）

用仓库外最小探针 `aster.ws.life`（SHA256 `172aec8dfa5b952a0af4c8760e514e5fc6b1d449ed0c8d2b5eeba48d52175393`，不进 SDK 产品示例）在两板诊断固件上执行，事件序列逐条来自串口日志（`lckfb-ws-life-r3.txt`、`wave-ws-life-r1.txt`）：

1. **握手中取消**（对端 `/stall` 不回 101）：`cancel-issued a=0`（受理），随即 `cancel-state a=3 b=0`——见"偏差"；`post-cancel-send a=-23`（终态拒发，映射正确）、`post-cancel-close a=0`（close 受理释放）。
2. **READY 带完成消息取消**：`cancel-at-ready a=0 b=1`（已收 1 条未取）→ `recv-1st a=0 b=11`：**已完成消息先于终态完整交付**、内容 `lifecycle-2` 匹配（无 content-mismatch 日志）；随后 `close a=0`。
3. **trap 回收**：新句柄 READY 后 `trap-now` 执行 `__builtin_trap()`。APP 状态 `CRASHED err=14`，同轮核实 `WS active=0 closing=0 tls=0 psram=0`、`NET active=0 opened=closed=N`、传输与单客户端配额立即可用（CLI 新开 `WS handle=7/4` 成功）。两板序列一致（phase2/3 连接等待时对上 `/echo` 的轮询时机使个别句柄号不同，语义相同）。

**偏差（如实记录，两板一致复现）**：同一帧内紧接 `cancel` 之后的查询看到的仍是**取消前状态**——phase 1 `cancel-state a=3 b=0`（仍 UPGRADE、error=0，非预期 8/-19），phase 2 紧接取消后第二条 `recv-2nd a=-15`（EAGAIN，而非立即 ECANCELLED），`post-cancel-send a=-23`（仍按握手前状态拒发）。根因是 `ws_end()` 只置 `c->cancel` 标志并唤醒 worker，终态转换发生在下一次 `aster_ws_svc_tick`，因此 **cancel 的可见性有一个 worker 周期的窗口**；本探针未再轮询到终态即 close。这与资源回收无关（close/stop/trap 后配额、句柄、PSRAM 均实测归零），也与主机桥接 fixture 不矛盾（fixture 的 CANCELLED 断言在 pump 推进状态机之后）；G1 CLI 用例先轮询到终态才断言，因此未暴露该窗口。问题在于公开契约文案写明"cancel 保留 CANCELLED 终态直到 close"，未声明 APP 同帧立即查询可能读到旧状态；APP 正确写法是像对待 EAGAIN 一样下帧重试 status/recv。处理决定：不改产品代码掩盖，记为待评审项——要么在 `ws.h`/手册 19 节补一句"终态以轮询观察到为准（可能滞后一个后台周期）"，要么让 cancel 在受理时同步置终态并补回归；与 N5.2 一并决定。专项证据链本身完整：受理 OK、完成消息先于终态交付（recv-1st OK/11B/内容匹配）、trap 后 CRASHED err=14 且同轮全部资源归零、新会话可立即建立。

清理：探针卸载、`/SD:/aster.ws.life.ast` 删除；双板恢复日常固件（hash 核对、abi 3.11.0、LCKFB 17 APP/微雪 14 APP，均无 life 残留）；Node 对端精确停止（监听归零）；临时防火墙规则删除并核实计数 0；串口释放。探针工程与包只存 `diagnostics/network-n5-g2-20261017/ws-life*`，未进入 Git。
