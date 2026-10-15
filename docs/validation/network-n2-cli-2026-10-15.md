# N2 HTTP/HTTPS CLI 验证（已完成，2026-10-15—16）

两板CLI功能、证书拒绝、资源回收、断网恢复及图形交互验证通过；用户均确认正常。本记录保留ABI3.8阶段诊断镜像与实测口径；随后完成的ABI3.9 APP/SDK验收见[配套记录](network-n2-sdk-2026-10-15.md)。本轮未发布Release。

## 微雪 3.5B

诊断镜像 SHA256：`66225417c4badb6afdf19c4d3b6c275529acdece3d3f19b32f95ecaf85d6dbcf`。使用微雪日常板级配置，追加 `config/diagnostics/network-http.conf` 和 `config/diagnostics/network-pools.conf`。屏幕仍为 RGB565、40 MHz、内部 RAM 双缓冲预拷贝。测试后源码将私有服务入口统一命名为 `aster_http_svc_*`，该命名调整未改变本镜像的 CLI 协议。

- 21 个功能用例通过：固定长度、64 KiB 慢读、chunked、EOF、HEAD、POST、404、500、重定向、截断、响应上限、总超时、有效 HTTPS、HTTPS chunked、错误主机名、过期证书、不可信根、证书故障后恢复，以及 HTTP 等待、TLS 握手、队列满时取消。
- 每段 CLI 读取检查 CRC32，完整响应检查 `i % 251` 模式和 SHA256。64 KiB 响应 SHA256 为 `4b640d85ab3ba30fd02c9fc9db4a8928f416322ad27022ea58a65aaee68a4df2`。
- 慢读项包含主动停读 2 秒和串口十六进制输出，用时 16.060 秒，因此使用 60 秒总期限。此前 15 秒期限版本在 58448 字节处按设计超时，不能将它记作内容损坏，也不能用该串口采集时间衡量网络吞吐。独立 1 秒截止用例仍验证总超时。
- 错误主机名、过期、不可信根分别报告验证标志 `0x4`、`0x1`、`0x8`，均拒绝请求。没有跳过证书验证。
- 结束后 HTTP 和原生服务均为 active=0、closing=0、私有 PSRAM=0，原生 opened=24、closed=24。取消计数 10 包含取消和错误路径，不代表十次连接故障。TCP FIN_WAIT_2 曾短暂保留；后续 `net conn` 确认无 TCP 连接，只余系统 DNS 上下文及 DHCP 处理项。

## 实测资源

- TLS/PSA 全局池预留 98304 B，局部证书矩阵峰值 39868 B，清理后当前分配为 0；另一次公网证书测试峰值 51924 B。它与包验签共享，不能理解为未来任意证书链的上限。
- HTTP 工作栈使用 3364/8192 B，原生网络工作栈 1316/4096 B。
- 一个普通 HTTP 请求对象 21264 B PSRAM；原生 TCP 缓冲另占 4096 B，TLS 上下文、自定义 CA 另计。96 KiB 加密池为静态 PSRAM 保留。
- 内核堆 size=98304 B、peak=41764 B；清理后 used=15276 B、free=82804 B。
- libc 池 usable=112252 B、peak=67284 B；清理后 used=64184 B、free=48068 B。后台服务栈仍保留，不应期待回到首次创建线程前的用量。
- 网络包描述符 RX 峰值 4/8、TX 2/8，数据分片 RX 14/24、TX 4/32；分配统计只有 OK，无 FAIL。数值为本次启动累计高水位，不能解释为 HTTP 独占用量。
- 后台名义 5ms 采样观察上下文峰值 3/8，是采样下界，未覆盖本轮 SNTP 重叠。独立 HTTPS+UDP 双槽检查已通过；本轮不替代 N1 的 DNS/DHCP/SNTP 并发预算验证。
- PSRAM 的 `static + wamr_cap` 是分区保留量，不是实际活跃分配。清理后最大可分配块探测至少 6 MiB，不能把输出中的 96% 直接解释为仅剩 4% 可供 APP 使用。

## 其他已通过的微雪检查

Gfx Show 前台动画期间，后台 HTTPS 校验 40800 字节并完成 34 轮 UDP 自环校验；用户确认画面、触摸及上下拉恢复正常。断开 Wi-Fi 后，请求在 8 秒期限内失败，缓冲中的 320 字节仍可先读取并校验；异步重连状态成功后，HTTP 和 HTTPS 各 4096 字节校验通过。

默认根证书下 `valid-isrgrootx1.letsencrypt.org` 返回证书验证成功的 HTTPS 200；`letsencrypt.org` 另一次测试在握手阶段遇到底层 EPIPE，仍是兼容性未解项。默认只内置 ISRG Root X1，不能宣称兼容全部 HTTPS 站点。

## LCKFB

诊断镜像 SHA256：`aef7d1ae357ba133a4256f66d010cfdd6f7f55ba73e7f6d91f1224ed3a35c519`。板型为 `aster.esp32s3.lckfb`，使用立创日常配置追加相同 HTTP 和协议池诊断片段。原有 17 个 APP、音量 74 保留；联网后开始测试。

- 同一套 21 项 HTTP/HTTPS 功能矩阵全部通过，完整响应均核对内容。TLS 错误主机名、过期、不可信根验证标志分别为 `0x4`、`0x1`、`0x8`。
- 独立配额验证通过：HTTPS 握手等待与 UDP 占两槽时，第二个 TLS 请求返回 EBUSY、第三个普通请求返回 ENOSPC，使用期间更换 CA 返回 EBUSY。关闭后可重新完成有效 HTTPS 请求。
- 断网测试通过：读取慢响应期间断开 Wi-Fi，请求在 8 秒总期限内失败；先读取并校验已缓冲的 320 字节，再获得终态错误。保存配置异步重连完成后，HTTP 与 HTTPS 各 4096 字节校验通过。
- 功能矩阵结束 HTTP/native active=0、closing=0、私有 PSRAM=0；native opened=22、closed=22，failed=0、cancelled=9、dropped=0。这里的计数只覆盖该轮矩阵，不包含后续独立配额与断网测试。
- 加密池峰值 39868/98304 B，清理后当前分配 0；HTTP 栈峰值 3364/8192 B，网络栈 1316/4096 B。
- 网络包描述符峰值 RX4/8、TX2/8，数据分片 RX14/24、TX4/32，分配无 FAIL。上下文采样观察峰值 3/8；未捕获 SNTP 重叠，口径与微雪相同。
- 内核堆 size=98304 B、peak=35660 B，矩阵结束 used=15316 B、free=82756 B；libc usable=110616 B、peak=68308 B、used=68308 B、free=42308 B。原生文件传输 worker 已创建并保留其 4096 B 栈，不能与未创建它的微雪快照直接作差认定泄漏。
- 清理后 PSRAM 最大可分配块探测至少 6 MiB。
- 2048 前台保持 RUNNING，后台 HTTPS 完整校验 65536 字节，同时完成 139 轮 8 字节 UDP 自环内容校验。自动检查结束关闭测试请求，2048 继续前台运行；实体画面和上下拉由用户确认正常，随后恢复日常固件和2048。

## 复现和证据

使用 `tools/validation/network_http_server.cjs` 的 Windows 原生服务器，并运行：

```sh
python tools/validation/network_http_capture.py \
  --sdk-tools ../aster-sdk/tools --port /dev/ttyACM0 \
  --host <电脑局域网IPv4> --output <仓库外结果目录> --pools
```

测试 CA 公钥事先放入 `/SD:/aster/n2-ca.pem`；私钥只在主机仓库外。微雪复测结束已恢复默认信任根并删除该临时文件。原始串口、逐项 JSON 和构建/烧录记录在工作区仓库外 `diagnostics/network-n2-20261015/`；不纳入 Git。

微雪已恢复日常镜像 `a11cb91c704eef3aefbd7b2ae17ed3ee12f0560426ca3f1a2d74af24e354e757`，确认 HTTP 和协议池诊断关闭。原有 13 个 APP 保留，已保存的 Wi-Fi 自动连接成功；旧版 Gfx Show AOT 启动并确认 RUNNING，保持前台。这是CLI阶段的恢复镜像；最终ABI3.9日常镜像和APP恢复见配套记录。

CLI阶段固件主机回归103项通过；加入公共桥接后104项通过。CLI、APP/SDK均已完成本阶段双板验收。N4长时复合压力及未解公网站点兼容性保留为独立边界。
