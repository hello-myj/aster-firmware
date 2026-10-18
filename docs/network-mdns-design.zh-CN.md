# N5.2 mDNS 设备名称发布（设计方案，实施前评审稿）

日期 2026-10-18，基线 `feature/network-n5`（ABI 3.12）。本文固化 N5.2 的范围、架构与预算决策；实施按实施方案 10.0 节 G1→G2 推进。用户已确认口径：**纯自动开关（A）、只做 mDNS 名称发布、不做 DNS-SD browse/查询、不做 APP API**。

## 1. 范围

**做**：设备在 station 联网且用户处于 Wi-Fi 配网界面期间，通过 mDNS（RFC 6762）发布一个 A 记录：`<hostname>.local → 当前 IPv4`，响应局域网内的名称查询，并可主动 announce 供手机"设备发现/投屏配对"类工具使用。
**不做（本轮）**：DNS-SD PTR/SRV/TXT 服务枚举与 APP 查询接口（若 N5.3 本地 HTTP 需要发现，届时按 G 流程扩展）；LLMNR；IPv6；热点(AP)接口上发布；常开模式设置项（方案 A：仅配网触发）。

## 2. 为什么不用现成组件（决策记录）

- Zephyr `CONFIG_MDNS_RESPONDER`：SYS_INIT 开机常驻、`select NET_MGMT_EVENT` 接口事件驱动、自带 `mdns_work_q` 线程，且仅回 A 记录无 announce 策略钩子——违反"不新增常驻线程、随模式启停"两条硬约束。其 Kconfig 拉起的 `net_socket_service`/`NET_MGMT_EVENT` 线程在现有构建中本已存在（`CONFIG_NET_SOCKETS_SERVICE=y`、`NET_MGMT_EVENT_THREAD=y`），但那不是我们新增 mDNS 线程的理由。
- Zephyr `CONFIG_DNS_SD`（`dns_sd.c/dns_sd.h`，RFC6763 报文助手）：纯函数库，不依赖 responder 符号，可在启用 responder 关闭的情况下单独编入。**本轮结论：连它也不需要**——范围砍到纯 A 记录后，仅需手写固定的 DNS 响应报文（问题名原样回显 + PTR 无关 + 单 A RR），编解码量约 200 行；避免为一个 helper 打开新 Kconfig 面。若日后扩 DNS-SD 再引入。

## 3. 架构（aster 自有有界服务，方案 B 线程模型）

新文件 `systems/zephyr/services/aster_mdns.c/.h`：
- **生命周期**：`aster_mdns_enable()/disable()` 由 Wi-Fi 配网 UI 进入/退出调用（`wifi_settings.c` 的 `aster_wifi_settings_open/deleted` 两处，`#if IS_ENABLED(CONFIG_ASTER_MDNS)` 包裹）。enable 只做置标志 + 唤醒 worker；全部 socket 动作在 `aster_net` worker 内完成（同 WS 并入 HTTP worker 的先例），**0 新线程、0 新定时器**。
- **socket 与名额**：内部 owner（与 `ASTER_WS_CLI_OWNER` 并列的系统内部 owner 常量），在 `aster_net` 的 **2 个 APP/CLI 传输槽之外** 增加 1 个系统保留槽（`ASTER_NET_SLOTS` 旁挂专用槽或扩为 3 并保证 APP 只见 2）——实施时选前者：独立 `struct mdns_ctx` 自持 fd，仅借用 worker 循环与 `net_wake` 信号，不复用 APP 槽记账；`aster net stats`/`netpool` 增列 `mdns` 行。UDP bind `0.0.0.0:5353` + `SO_REUSEADDR`，`IP_ADD_MEMBERSHIP` join 224.0.0.251（当前 `net_if_get_default()` 的 station 接口），disable/接口 down 时 `IP_DROP_MEMBERSHIP` + close。
- **状态机**（全在 worker tick，非阻塞）：`OFF → PROBING?(可选,见 §5) → IDLE ←→ ANNOUNCING`；收到查询 → 解析问题名 → 匹配则回单播/多播 A 响应（TTL 120s）；不匹配回 NSEC 空答或直接静默（首轮静默，边界测试再议）。
- **身份**：hostname = 出厂常量前缀 + 站点接口 MAC 尾 3 字节十六进制（`net_if_hwaddr`，小写），如 `aster-1a2b3c.local`。MAC 不可得时拒绝 enable 并在 CLI 报错，不静默回退随机名。IPv4 每 tick 用 `net_if_ipv4_get_global_addr` 现取，不缓存旧地址；DHCP 续租换址自动生效。

## 4. 边界与资源预算（G1 实测对账项）

| 项 | 预算 | 依据 |
|---|---|---|
| 关闭态（日常） | PSRAM 0、无 socket、无多播成员、worker 不轮询（`active=false` 睡 `K_FOREVER`） | `CONFIG_ASTER_MDNS` 默认 n + 运行期 enable 才分配 |
| 开启态 | 1 socket + RX/TX 各 1×512B 静态（PSRAM 分配一次，<1.2KiB）+ ctx 数十 B；合计 **<2KiB**，远低于 12KiB 上限承诺 | mDNS 单报文 ≤1472B，问题名 ≤255B；固定尺寸缓冲 |
| net 池 | `net_pkt/net_buf` MaxUsed 观察 +0~2（多播入包），不改池配置 | G1 用 `net mem`/`netpool` 前后对比 |
| 栈 | `aster_net` worker 水位增量实测记录（基线 1316/4096=32%） | tick 内全部固定尺寸局部缓冲 |
| DRAM | +1 fd 与 socket 对象（协议栈内部，预计 <1KiB） | 同 UDP 槽 |

## 5. 名称冲突（RFC6762 §8.3 的有界简化）

完整 probe/被动探测状态机本轮不做（它需要多轮定时与记录缓存）。采用：enable 后先发 1 组 announce（2 连发，间隔 1s），并在 IDLE 监听期间**被动让位**——若收到同名 A 应答且源 IP ≠ 本机，则追加随机后缀 `-2`…`-9` 重启一次；再冲突则报错退出（CLI 可见）。G1 用两台同 MAC 尾模拟（测试构建可注入伪造名）验证。若验收认为不足，再升级完整 probe，届时单列设计。

## 6. CLI（仅 `CONFIG_ASTER_MDNS=y`）

```
aster net mdns on|off          诊断手动强制（与 UI 门叠加：任一要求开则开）
aster net mdns status          state/name/ip/if/index
aster net mdns stats           rx/tx/drop/conflict/since
```
默认信任 UI 生命周期；CLI on 只为桌面/局域网自动化验收存在，重启后不残留。

## 7. 验收（G1，两板）

1. 进配网页 → 手机 `dns-sd -B`/`ping aster-xxxxx.local` 解析成功；退页 → TTL 后不可解析、设备侧 socket/成员/PSRAM 归零（`aster net mdns status`+`aster net stats`+`aster sys mem`+`netpool`+`net mem` 五样前后对账）。
2. 断网/重连：DHCP 换址后名称解析到新 IP；station down 时自动 drop 并静默，up 后重新 join（`hw wifi disconnect/reconnect` 注入）。
3. 多播不可达回退：对端静默时设备不崩、计数可见、退出模式即清零——"IP 手动回退"属 N5.3 配网页职责，本项只证明发现失败不拖垮系统。
4. 冲突让位一次；同名双实例场景注入。
5. 资源：开启/关闭各采一次非原子快照，记内核堆/libc/PSRAM/线程栈四项与 netpool；与 §4 预算对账，超预算须解释或收缩。
6. UI 并发：配网页正常滚动/输入不受 tick 影响（worker 与 UI 本就分线程）。

## 8. 风险与留待决定

- **发布时机是否够**：若实测发现"手机在配网前就需要发现设备"（投屏场景），需回头扩"常开"开关——那是产品决策，当前按 A 执行并记录假设。
- AP 接口：热点期间 AP 子接口是否 join 多播待硬件验证；首版 station-only。
- mDNS 报文解析面小（仅取 question name），但畸形包/压缩指针对象必须进主机 fuzz 测试（仿 `ws_codec_host.c` 模式），不允许递归解压炸弹。
