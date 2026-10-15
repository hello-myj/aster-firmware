# TCP/UDP 网络服务与 CLI（N1 实施中）

2026-10-15：这是固件共享网络服务的诊断入口，不是 APP 公共 ABI。工作区公共ABI已升至3.8.0并接入SDK/模拟器的aster_network_*接口，已发布3.7包不含这些导入；两板APP验收仍在进行。完整阶段验收见 [实施方案](network-services-implementation.zh-CN.md#n1契约资源模型与-tcpudp)；未通过两板 G1/G2 前，不将 N1 标为已完成。

## 构建与组成

两板 `base.conf` 开启 TCP；满足 socket、DNS、TCP 配置且 Wi-Fi 启用时，`CONFIG_ASTER_NET` 默认启用。可显式关闭该选项，移除服务与命令；底层协议栈配置独立存在。

- `systems/zephyr/services/aster_net.c/.h`：共享资源、状态机、异步 DNS、socket 收发和生命周期；不调用 shell、LVGL 或 WAMR。
- `systems/zephyr/shell/aster_net_cmd.c`：参数校验、CLI owner 和有界输出，调用同一服务。
- `tests/net_service_host.c`、`tests/net_host_platform.h`、`tests/test_net_service.py`：实际服务 C 的 POSIX 回环测试；DNS/时钟可控，用 ASan/UBSan 检查内存。宿主机回环不等于 ESP32 Wi-Fi 链路验收。

## 命令

先通过既有 Wi-Fi 设置连接路由器/手机热点，再查询地址；`hw wifi reconnect`异步提交已保存配置的重连，`hw wifi status`显示操作结果。网络命令不主动扫描、更换 Wi-Fi 或重连。个人热点仅提供局域网，不提供 NAT。`status` 中 `up` 是接口启用，`carrier` 是底层载波状态，`address` 表示已取得 IPv4，均不证明互联网可达。部分Wi-Fi驱动在未关联时仍保留carrier标志，因此这里的await-ip也可能表示尚未关联；无线关联状态及连接任务结果请结合 `hw wifi status` 查看。

```text
aster net status
aster net stats
aster net tcp connect --host 192.168.1.10 --port 9000 --timeout-ms 5000
aster net result 1
aster net tcp send 1 --hex 000102ff
aster net tcp recv 1 --max-bytes 128
aster net cancel 1
aster net result 1
aster net close 1

aster net udp open
aster net result 2
aster net udp sendto 2 --host 192.168.1.10 --port 9001 --hex 000102ff
aster net udp sendto 2 --host 192.168.1.10 --port 9001 --hex -
aster net udp recvfrom 2 --max-bytes 1200
aster net close 2
```

IP/端口是示例；句柄以 `NET handle=... queued=1` 实际输出为准，不保证从 1 开始。TCP 默认连接总超时 5000 ms，可设 1..30000 ms；包含 DNS 与 connect。其余数字为十进制。当前 `result` 不接受 operation 参数；HTTP/MQTT 尚不存在，不要直接使用实施方案中的后续草案命令。

命令只提交/查询操作，不等待远端。`queued=1` 或发送返回长度表示进入本地队列；随后查询连接状态、实际发送统计，并核对接收内容，不能据此宣称远端收到。自动串口脚本按 `NET` 记录及随后的 shell 提示符判断命令完成，不依赖长命令的完整回显（终端折行/游标重绘会改变回显）。CLI 输出采用 `NET` 前缀，错误值在 `result`/`error` 中；传输错误不会再触发 shell 打印一遍用法。shell 参数个数错误仍由 Zephyr shell 处理。

`recv` 首行包含 `result`、`required`、来源和 CRC32，成功数据另按每行最多128字节输出 `NET data offset=... hex=...`。发送命令每次最多96字节十六进制数据（与253字节主机名组合时，整行仍能容纳在512字节 shell 输入缓冲），`-` 表示空数据；接收容量最多1200字节。CRC32 是接收这次返回的字节，不是整个 TCP 消息的隐含校验。CLI 接收临时缓冲也使用 PSRAM，返回前释放。

## 状态、错误和读取规则

连接状态：`opening`、`resolving`、`connecting`、`ready`、`eof`、`failed`、`cancelled`。`phase` 保存最后执行/失败的动作：1 socket、2 DNS、3 connect、4 TX、5 RX、6 cancel。

当前内部服务/CLI 返回负 Zephyr errno，**不属于 Aster 公共错误枚举**。常见值为 `-EAGAIN`（暂不可用/背压/服务锁忙）、`-EBADF`（非法、跨 owner 或已关闭句柄）、`-ENOSPC`（两槽已占用）、`-EMSGSIZE`（包/接收容量不符）、`-ETIMEDOUT`、`-ECANCELED`。SDK 桥接阶段必须映射稳定公共错误，禁止将这些平台数字直接作为 APP ABI。

- TCP 是字节流：两次 send 可以合并，一次 send 可以拆开。发送最多接受当前空位的字节数，其余由调用者稍后补交；返回 `EAGAIN` 时未接受数据。挂起发送在连续5秒没有发送进展后失败；读取不阻塞，暂时无数据返回 `EAGAIN`。
- TCP 接收容量必须大于0。先返回已缓存数据，再返回失败/取消；对端有序半关闭且缓存空才返回0。Zephyr 4.4.2 原生 TCP 不支持半关闭后继续发送；首版统一在观察到 EOF 后拒绝新发送并返回 EPIPE，宿主机/后续模拟器也遵守此限制。已排队但未发完的数据遇到 EOF 时报告发送失败。异常不伪装成 EOF。
- UDP 数据报原子入队，最大1200字节，可为空。接收容量不足返回 `EMSGSIZE` 和真实 `required`，原包保留；空包成功返回0，之后没有包则返回 `EAGAIN`。
- UDP 收发各两包，额外一个私有接收 scratch 包用于接收并统计溢出；超长包/队列满均丢包计数。发送任务从入队起最多5秒（含 DNS），不会因为前一包完成而重置后一包的等待时间。首版发送错误使该端点进入失败，重开由调用者决定；不自动重发。
- 首版 IPv4 单播；拒绝0网段、多播和保留高地址。未开放广播、多播、TCP 监听或原始报文 API。
- 取消保留句柄供查询及读取已缓存输入，丢弃尚未发送数据；已失败任务保留最初失败原因。关闭立即使句柄失效，重复关闭返回 `EBADF`，后台完成关闭后才归还槽和缓冲。

## 所有权、调度和预算

服务使用数字 owner/session，不持有 APP 对象或 WASM 指针。CLI owner 为1；同样受两槽全局预算约束。递增代数句柄不重复使用，旧 DNS 回调携带代数 token，不会写入新连接。代数到达 `INT32_MAX` 后拒绝再分配，而不是回绕。

只有网络 worker 持有并关闭 fd。DNS 提交/取消在服务锁之外执行，支持同步回调；取消用名称、类型与 DNS ID 共同定位。关闭标记期间槽仍计入额度，直到后台 DNS 已退出、fd 关闭、私有缓存释放。TCP 栈自身的 TIME_WAIT 由 Zephyr 管理，短时间大量重连仍可能遇到内核上下文不足；不将服务空槽视为所有系统上下文均已释放。

单个服务线程首次请求时创建，内部 RAM 栈4096字节，空闲且无槽时休眠，保留供后续复用。活动连接每轮最多处理有限数据并至多等待10 ms。服务调用者使用不等待的锁：忙时返回 `EAGAIN`，不等待 DNS/socket；APP 退出桥接后续必须重试 owner 清理，确认清理请求接受后才能退休该会话。当前未挂接 APP 生命周期，也未宣布 APP 后台通信可用。

TCP 每槽 RX/TX 各2048字节，共4096字节 PSRAM。UDP 包与描述符按平台对齐计算，`stats.psram` 是服务实际申请的字节总数，包含发送目标和额外 scratch；不是单纯 payload 长度。终态仍占槽和缓冲，调用 `close` 后才释放；`stats.active` 包含关闭中的槽，`closing` 是其中子集。常驻栈单独显示，不把它算作泄漏。

底层配置：8个网络 context、8个连接匹配条目、3个并发 DNS 查询；TCP 窗口各2 KiB、包分配等待10 ms。系统 DNS/SNTP/DHCP 与 Wi-Fi 自身仍消耗协议资源。这些是初始预留，不能据此声称两个 TLS 会话可用；TLS 尚未接入。

## 验证方法与边界

```bash
python -m unittest discover -s tests -p test_net_service.py
python -m unittest discover -s tests
python tools/check_docs.py
```

宿主测试使用真实 POSIX TCP/UDP 收发，并覆盖：部分接受、TCP 分段/合并与半关闭、发送进展超时、UDP 二进制/空包/1200字节/不足容量重试/超长及满队列、两槽配额、跨 owner/过期句柄、清理中的槽不复用、分配失败、同步 DNS 回调/取消/超时/失败/陈旧 token、200次创建关闭及全量缓冲归还。网络锁忙用注入校验返回 `EAGAIN`。

设备验收必须另做：可控局域网服务器逐字节/CRC校验、两槽并发、DNS及连接/断网异常、重复开关的两堆/PSRAM回收、网络操作期间触摸和上下拉响应、APP退出与重开（桥接阶段）、两板分别记录。临时 loopback 构建只验证设备本地协议栈，不代替 Wi-Fi/路由器。日志、截图、生成镜像和临时配置放在仓库外。

本轮实机记录见 [N1 验证记录](validation/network-n1-2026-10-15.md)。
