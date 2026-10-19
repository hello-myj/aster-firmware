# N5.5 网络状态事件与分阶段诊断

日期 2026-10-19，基线 `feature/network-n52`。范围取自实施方案 N5.5 四条。用户已确认内存口径（不影响 PSRAM，仅数百字节静态 DRAM）。

状态（2026-10-19）：主机与双平台模拟器零跳过通过。微雪 CLI 已覆盖成功连接、超时、取消、两槽拒绝和断网失败，并采到 DNS/连接样本；日常镜像与地址已恢复。LCKFB 未接入。实机事件仍缺 APP 消费端。详见 `docs/validation/network-n55-host-sim-2026-10-19.md`。

## 1. ABI 扩展（3.12 → 3.13，additive）

`contract/include/aster/event.h` 增加一个 sys code，**不新增事件类型、不改 `aster_event_t` 布局**（现有 `u.data[8]` 已够）：

```c
/* SYS notification: one network handle changed state. The event carries only
 * the handle and its new state; the APP reads details through the existing
 * status APIs (no payload, no borrowed pointers). Coalesced: at most one
 * event per (handle, state) transition, and never more than one pending
 * notification per APP turn. */
#define ASTER_SYS_NET_STATE 3u

/* u.sys.code = ASTER_SYS_NET_STATE
 * u.sys.value = handle
 * u.data[2]   = state  (protocol-specific state number, see each header)
 * u.data[3]   = kind   (1 TCP, 2 UDP, 3 HTTP, 4 MQTT, 5 WS)
 */
```

理由：不扩结构即可携带 handle+state+kind；`ASTER_EVENT_SYS`(5) 通道此前已定义但无生产者，直接启用。

## 2. 事件产生与合并（固件）

- 位置：`app_event_loop` 中 frame 事件之前，调用 `aster_net_events_pump(ctx)`。
- 每个协议服务在其 tick 里记录"状态变更"，写入一个**全局有界通知环**（固定 8 条），只有 `(handle, state)` 变化时才写入；同句柄同状态重复不发。
- `pump` 每次最多投递 **1 条**给 APP，保证单帧耗时有界；剩余留到下帧。APP 不消费时环只保留最新变化（覆盖最旧），不无限增长。
- 事件只对**拥有该句柄的 APP 会话**投递（owner 校验），CLI 句柄不投递。
- APP 停止/trap 时清空其待投递项。

## 3. 分阶段耗时诊断（内部，不进 ABI）

在既有结构上**加字段**，不新增分配：

| 结构 | 新增字段 | 大小 |
|---|---|---|
| `struct slot`（net） | `dns_us`, `connect_us` | 8 B × 2 slot |
| `http_request` | `tls_us`, `ttfb_us`（首字节） | 8 B × 2 实例 |
| `ws_client` | 复用 http 的 TTFB（升级耗时） | 0 |
| 全局计数 | `net_diag` 统计最大/平均 | ~64 B |

`aster net diag` 新子命令输出：DNS/连接/TLS/TTFB 的 `last/avg/max`（毫秒）。**不轮询外网**，全部来自真实请求样本；样本数为 0 时显示 `-`。

## 4. shell 诊断增强

`aster net stats` 追加：`apps=<n>`（APP 会话数）、各协议 `queued/closing` 水位、`failed/reconnect` 计数、`dram_peak`/`libc_peak`。默认**不输出** header 内容或 payload（现有命令本就不打印）。

## 5. 验收（G1 主机 + 双板，G2 SDK/模拟器）

1. 主机 fixture：状态机走 CONNECTING→READY→EOF/FAILED，断言事件序列**去重**、owner 隔离、环满覆盖最旧、APP 退出清空。
2. 双板：断网/超时/配额不足/取消四种场景，事件数与最终状态一致；`aster net diag` 各阶段耗时有合理值；stats 字段非零正确。
3. 模拟器：同契约实现，双平台零跳过。
4. SDK：事件常量导出（仅常量，无新函数），示例 APP 在 `ws-echo` 或新页面演示"事件驱动替代轮询"。

## 6. 明确不做

- 不做外网探测/可达性检测；不做事件队列排队（只保留最新）；不把 payload 放进事件；不改变现有查询接口语义。