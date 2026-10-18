# N5.2 mDNS 名称发布 G1 验收记录（双板，2026-10-18）

承接[设计文档](../network-mdns-design.zh-CN.md)（方案 A：纯自动、仅名称发布、无 APP API）。代码版本：固件分支 `feature/network-n52`（自 `feature/network-n5` HEAD 起）；N5.2 无 SDK/模拟器改动。诊断构建：`build/network-n52-lckfb`（`840265f5…`→修 mreqn 后 `b840be03…`→最终 `a457bc3a…`）、`build/network-n52-wave`（最终 `2a2cb8be…`），配置 = LCKFB/微雪各自 production 基线 + network-http/pools/mdns.conf；`CONFIG_ASTER_MDNS=y` 仅诊断，`prj.conf` 不含（日常 0 代码，微雪恢复后 `mdns` 子命令消失为证）。

## 实现缺陷的发现与修复（均有证据）

1. **`IP_ADD_MEMBERSHIP` 结构**：本 Zephyr 的 join 路径只接受 12B `net_ip_mreqn`，8B `ip_mreq` 返回 EINVAL。实机 `state=1 error=-22` 暴露，改 mreqn+显式 ifindex 后 `state=3 IDLE`。主机 fixture 的 setsockopt 桩同步镜像该校验。
2. **应答端口 bug**：`reply` 后强制 `from.sin_port=5353`，把回包送进 Windows 系统 mDNS 服务（Bonjour/FD 独占 5353 入站），ephemeral 客户端永远收不到。改为按 RFC6762 回到查询源端口——修复后 PC 端 ephemeral 查询收到正确应答。
3. **出站组播出口**：补 `IP_MULTICAST_IF`（ifindex）+`IP_MULTICAST_TTL=255`。
4. **stale IP**：链路降级清空 `m.ip`，`ip=-` 不再显示旧地址。
5. 另修 `aster_net_cmd.c` include 嵌套错位与 FAILED 态 acquire 不自愈（主机测试覆盖）。

## 双板 G1 结果（各自独立，LCKFB `.99` / 微雪 `.92`，PC `.201`）

| 门 | LCKFB | 微雪 |
|---|---|---|
| 默认关/重启不残留 | `state=0 want=0 name=-` | 同 |
| CLI on→IDLE+2 次 announce | `state=3 announce=2 error=0`，名 `aster-2bddc8.local`（MAC 尾 3 字节） | `state=3 announce=2`，名 `aster-03bf10.local`（板间后缀正确不同） |
| 查询→应答（ephemeral 单播直查） | `MDNS_PEER_ANSWER ip=.99 ttl=120` | 同（ip=.92） |
| 手机场景（组播查询→单播回源端口） | 同证据 | 同证据 |
| 系统解析器 `ping <name>.local` | **解析成功 3/3 0% 丢包**（走 Windows FD，与手机同机制） | 原始报文中途未解析；判为 FD 修复前负面缓存的环境性干扰，以报文凭据与 LCKFB 系统解析为功能准，记录不隐去 |
| 被动冲突让位 | 注入 3 条同名异 IP 应答→`conflicts=1 renames=1` 改 `-2` 后缀且**只让一次**，再重发不二次改名 | 同（`aster-03bf10-2.local`） |
| Wi-Fi 断链降级 | `state=1 WAIT_IP`，`netpool udp=0`（socket+成员退出） | 同，且 `ip=-` |
| 重连恢复 | 自动回 `state=3`，**保留**改名后缀，重发 2 次 announce | 同 |
| UI 生命周期（方案 A 核心） | 进 Wi-Fi 设置页 `want=1 state=3`（~20s），退出即 `want=0 state=0 name=-`（0.5s 采样曲线，串口证据） | 同构曲线（`want=1 state=3`×73 样本）；用户实体操作确认 |
| 开/关资源账 | dram/libc/psram 逐项不变；netpool current +1（udp 1→2，与 NTP 并列系统上下文）；`aster_http` 栈 2356/8192（28%，含 mDNS tick） | 开/关 mem 快照逐行相同，仅 netpool +1；观察一致 |

**未闭合项（如实）**：设备**主动 announce 组播**未被 PC `observe` 监听器捕获（sendto 全部成功、入向组播正常、单播应答正常；疑似 AP 对设备→组播的转发/监听器与系统 5353 共占相关）。发现链在实测语义上依赖"查询→应答"，手机/PC 主动查询场景已由 LCKFB 系统 `ping` + 双板报文凭据覆盖；N5.3 配网页联调时用真机手机复验一次，仍不通则将周期重发 announce 降级为可配置项。

## 收尾

双板恢复日常固件并核 hash（`aa638110…`/`6291edaf…`），无测试 APP/CA（本轮未用）、`mdns` 子命令消失；Windows UDP 5353 临时规则经管理员删除核实计数 0；无对端常驻进程（peer 均为一次性调用）；串口释放。工具：`tools/validation/network_mdns_peer.cjs`（query/query-ephemeral/answer/conflict/observe 五模式，dgram only）。主机 124 项（新增 `test_mdns`/`mdns_host.c` ASan/UBSan：名称编解码、压缩指针拒绝、截断/畸形/随机前缀 fuzz、应答构建字段、owner 位与 FAILED 不自锁）。原始日志在仓库外 `diagnostics/network-n52-20261018/`。

N5.2 状态：G1 硬件门全部有证据（含一项环境性未闭合），**双板逐项复核完成前不标 N5.2 完成**；N5.3 依赖的"发现失败退回手动 IP"路径本记录已给出降级证据（发现失败不影响系统）。改动未提交（`feature/network-n52`，等用户指示）。
