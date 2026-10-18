# N5.1 WebSocket/WSS CLI 双板 G1 验收记录（2026-10-17 续）

本记录承接[首轮微雪记录](network-n5-ws-cli-2026-10-17.md)，补齐 LCKFB 的 G1，并按实施方案 10.0 节补做双方缺失的边界用例。代码版本：固件分支 `feature/network-n5`，基线提交 `c170db0`（N4 为 `8a71fd2`）；本轮对局域网 fixture 增加 `/slow`、`/slowfrag`、`/multi`、`/ping`、`/blackhole` 路由，并新增补充门禁脚本 `tools/validation/network_ws_g1_extra.py`（均为测试工具，不属产品代码）。SDK、模拟器无 N5 增量。

## 主机与设备接入（WSL usbipd）

- 板型以设备响应确认，不以串口名或会话编号推断：`aster sys info` 分别返回 `aster.esp32s3.lckfb`（控制台 `/dev/ttyUSB0`，CH340 bus 3-4）与 `aster.esp32s3.waveshare35b`（`/dev/ttyACM0`，bus 1-3 原生 USB）。
- LCKFB 原生 USB（bus 3-3，`/dev/ttyACM1`）应答 ASTERPING 且 `serial=shared`，为文件/CLI 共享通道之一；控制台仍在 UART 桥 `/dev/ttyUSB0`。CH340 的 usbipd `bind` 需管理员权限，经用户 UAC 批准后 `attach --wsl` 生效。
- 局域网：PC `192.168.110.201`（Private/WLAN）、LCKFB `192.168.110.99`、微雪 `192.168.110.92`，同网段互通。证书复用 `certs/`，valid SAN 覆盖当前 PC IP，wrong 名称不符、expired 已过期，三者由测试 CA 签发。临时防火墙规则（TCP 17880-17885，Private/LocalSubnet）经管理员创建，本轮结束后已删除并核实（不再遗留）。

## 构建与刷入

- LCKFB 诊断目录 `build/network-n5-lckfb` 增量构建（沿用 `CONFIG_ASTER_HW_LCKFB_S3=y`、微雪选项关闭、`network-http/pools/websocket.conf`），镜像 SHA256 `f9667a8149b9c9bac5bc089e5ed6fd656424fc9ef9a22b0c3bbb3796794a590b`；刷入前留存日常镜像哈希 `aa6381107596340fc0ea58d8f6aa3f2437ef8650a4ba51edfad48adbb7afb4f1`。
- 微雪诊断目录 `build/network-n5-wave` 增量构建（板型 `waveshare_esp32s3_touch_lcd_35b`，与缓存一致），日常镜像 `build/qspi-prefetch40` 哈希 `6291edaffaca107e8baae63ee6b185ac56211363a6695ddbb350cd8401d94057` 核对一致。
- 主机 122 项测试在实施前重跑通过（18.854 秒），含真实 C 服务/编解码 ASan/UBSan；主机 TLS 桩不等同实机 TLS 验证。

## LCKFB 基线（本轮实测，不照抄 N4）

17 个 APP 全部 STOPPED、音量 74、Wi-Fi 已连接、内核堆 used=15352、libc peak=59064、PSRAM 静态 274496、网络计数全零。基线文件 `baseline-lckfb.txt`（仓库外）。

## LCKFB G1 结果

1. **基础 CLI 门禁**（`lckfb-cli-r1`，exit=0 且内容核对）：15 类全部通过——WS/WSS 文本/二进制/空消息回环与 4096 边界、4096 对 17 短缓冲两次取回报 `(i+17)%251`、WSS+HTTP 共享准入与 16384 字节内容校验、分片穿插控制帧（"中"）、badaccept/oversize/badutf8/masked 拒绝、错名称与过期证书拒绝（verify=4/1，error=-13）、明文与 TLS 停滞握手超时（-116）、对端 close=1000、空闲 ping/pong 保活、升级期取消（-140）。资源样本：明文 WS 私有 PSRAM 18040 B，WSS 19887 B，WSS+HTTP 并发 TCP 槽 8192 B——与微雪完全一致；结束时 WS active/closing/tls/psram、HTTP、NET 全回零，opened=closed=16。
2. **断网恢复 + 图形并发**（`lckfb-ui-r1`，`WS_UI_PASS rounds=85 http_rounds=6`）：断开-重连后新 WSS 真实消息校验通过；前台 2048 RUNNING 下 85 轮 WSS 回环与 6 轮完整 16 KiB HTTP 内容校验持续通过。READY 时快照：WS psram=19887、HTTP 23312、NET active=2、内核堆 16200、libc peak=79628、NETPOOL current=3/limit=8。**用户实体确认：滑动 2048、上拉取消、下拉再收回画面与触摸全部正常；期间无人为拔插 USB。** 收尾句柄/PSRAM/TLS 归零、CA 恢复默认、测试 CA 删除。
3. **G1 补充门禁**（`lckfb-cli-r4` 与 `wave-cli-r4`，各 13 项通过）：DNS 失败（-121）、端口拒绝与 TEST-NET 连接期限（-116）、连接期取消、READY 期取消（-140）、关闭后陈旧句柄拒绝（-9，句柄单调代次不复用）、慢速逐字节帧有界完成、慢消费有序批含空消息与短缓冲不消费（-122 报告 required 后完整取回）、控制帧穿插时客户端自动 pong 闭环（对端收满 3 pong 回终结标记）、单客户端耗尽拒绝第二 open（-28）且关闭后可再开、分片停滞 30 s 组装期限（-116）、黑洞对端空闲保活期限（-116）且 CLI 保持响应、25 轮开-收-关生命周期后 WS/NET/NETPOOL/内核堆/libc/PSRAM 回基线。

## 微雪补做与已知限制

- 微雪重刷同一诊断镜像补跑 13 项补充门禁全部通过，行为与错误语义同 LCKFB 一致；微雪历史 15 类基础与并发结果见首轮记录。失败历史（`lckfb-cli-r2/r3`：脚本等待条件与单槽设计不符）为测试脚本缺陷，修正后完整重跑，未据此改动产品代码。
- 已知设计边界（非缺陷，测试证实）：单个待取消息槽使未消费数据帧暂时阻塞后续数据帧（头队阻塞有界，控制帧在其后帧到达时仍被处理）；30 s 分片与保活期限不依赖 TCP 活性。
- 局限：以上均为局域网有界串口校验，不是带宽/长时压力测试；补充门禁未含证书/内容全矩阵（由基础轮覆盖）；两板并发轮各只有一轮；主机 fixture 100 轮不替代实机数据。`ws-graph` 循环约 0.35 s/轮受串口往返限制。
- G1 已通过（双板 CLI+图形并发+补充边界+实体反馈）。G2（契约、native/WAMR、SDK、示例 APP、模拟器、双板 APP 验收）未开始；公共 ABI 仍 3.11，`CONFIG_ASTER_WEBSOCKET` 默认 n，不得宣称发布 SDK 已支持 WS。

## 收尾状态

双板已恢复日常固件（哈希核对、板型纯、诊断关闭）、原 APP/音量/网络保留、前台回启动器；测试句柄归零、CA 删除、默认信任源恢复；本轮 Node fixture 已停止（仅杀该命令行进程）、临时防火墙规则已删除并核实；串口已释放。原始日志、证书与镜像在仓库外 `diagnostics/network-n5-lckfb-20261017`（另含 `network-n5-20261017` 首轮），不入 Git。
