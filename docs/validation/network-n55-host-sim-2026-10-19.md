# N5.5 主机与模拟器验收（2026-10-19）

承接[设计文档](../network-n55-design.zh-CN.md)。分支 `feature/network-n52`，三仓均未推送。本记录只覆盖已经跑完的检查。双板场景（断网、超时、配额、取消，以及带样本的 `aster net diag`）还没做完。

## 已通过

- 固件主机：`python -m unittest discover -s tests`，125 项，OK。
- SDK：105 项，OK。手册与 Skill 的当前 ABI 已改为 3.13.0，并写明 `ASTER_SYS_NET_STATE`。
- 模拟器 Linux（Node，本机夹具，`ASTER_HTTP_TEST_HOST=localhost`）：99 项，0 失败，0 跳过。
- 模拟器 Windows 原生 Node：99 项，0 失败，0 跳过。日志 `D:\tmp\aster-sim-g2-win\run-n55.log`。

Windows MQTTS 名称拒绝用例原先超时，原因是测试用的 `server.pem` 把 `IP:127.0.0.1` 放进了 SAN，按 `127.0.0.1` 连接会握手成功，用例一直等失败态。已重新签发，SAN 为 `DNS:localhost`、`IP:192.168.110.80`、`IP:192.168.110.201`。证书在仓库外 `diagnostics/certs-n55/`，不进 Git。

## 微雪短时探针

诊断镜像 `build/network-n52-wave/zephyr/zephyr.bin`，SHA256 `4751520fb0c4468c9bed9753b369c4afda1bbd289051afafa8be4953e9287035`。刷入 `/dev/ttyACM0` 后：

- `board=aster.esp32s3.waveshare35b`，`abi=3.13.0`，`apps=14`。
- `aster net diag` 可执行，`samples=0`，四段耗时均为 0。
- `aster net stats` 含 `apps=0`，active/closing/psram 为 0。
- 链路停在 `state=await-ip`，日志有 `aster_wifi: async operation 4 failed: -116`。没有可用来采样 DNS/连接/TLS/TTFB 的地址，因此没有实机阶段样本，也不能做断网/超时/配额/取消。

探针后用 `--skip-rebuild` 恢复 `build/qspi-prefetch40`。恢复后核对：ABI 3.11.0，同一板名，14 个 APP，`aster net diag` 不再是独立子命令。日常镜像 SHA256 仍为 `6291edaffaca107e8baae63ee6b185ac56211363a6695ddbb350cd8401d94057`。

## 微雪 CLI（2026-10-19 晚）

诊断镜像同上，`/dev/ttyACM0`。启动后已关联已保存网络，地址 `192.168.110.39`，ABI 3.13.0，APP 仍为 14。串口记录在仓库外 `diagnostics/network-n55-wave-20261019/serial.txt`。

| 场景 | 结果 |
|---|---|
| 基线 | `samples=0`，`active=0 apps=0` |
| `www.baidu.com:80` | `state=ready error=0`，关闭后 `samples=1`，dns last/avg/max=17，connect=63 |
| `203.0.113.1:81` 超时 2500 ms | `state=failed error=-116` |
| 同一黑洞上 cancel | `state=cancelled error=-140`，stats `cancelled=1` |
| 两个 TCP 后再开第三条 | `NET result=-28`，随后两条都关闭 |
| 断开 Wi-Fi 再连域名 | `state=failed phase=2 error=-121` |
| 收尾 | `active=0 closing=0 psram=0`，`opened=6 closed=6 failed=2 cancelled=1 apps=0`，栈 4096 为网络 worker 常驻 |

关闭后 `connect last=13 avg=32 max=63`，`samples=3`。超时失败没有写入 connect 样本，因为连接没完成。tls/ttfb 保持 0：这次没有走 HTTPS，而且 HTTP 服务目前没有把这两段耗时送进 diag。

命令行把数值标成 `us`，计时来自 `k_uptime_get()`，单位是毫秒。17 和 63 应按毫秒读，不是微秒。

事件只投递给 APP owner。这次是 CLI 句柄，没有在实机上看到 `ASTER_SYS_NET_STATE`。

探针结束后用 `--skip-rebuild` 恢复 `build/qspi-prefetch40`。核对：ABI 3.11.0，14 个 APP，地址回到 `192.168.110.39`。

## 微雪补齐（同日）

诊断固件已含：诊断单位改为毫秒；HTTP/WS/MQTT 在握手完成时写入 TLS 耗时，HTTP 在响应头到达时写入首字节耗时，WS 在升级完成时写入；`aster net stats` 增加各协议 active/closing、MQTT 重连次数、DRAM 与 libc 堆峰值。`apps/http-fetch` 与 `apps/ws-echo` 收到 `ASTER_SYS_NET_STATE` 时写日志。

微雪上安装临时 `aster.http.fetch`（指向 `http://www.baidu.com/`，测完已卸载）。日志依次为 `NETSTATE kind=3` 状态 1（连接中）、3（发送）、4（响应头）、7（失败，正文超过 256 字节，`code=-20`）。关闭后 `aster net diag`：dns 34 ms、connect 94 ms、ttfb 230 ms。`aster net stats` 含 `NET http`、`NET mqtt reconnects=0`、`dram_peak`、`libc_peak`。停止后 active=0，卸载后仍是 14 个 APP。

明文请求没有 TLS 阶段，所以 tls 仍是 0。随后 CLI 访问 `https://www.baidu.com/` 被设备信任库拒绝（`verify=8`，`tls_error=-9984`），握手没有完成，因此也不记入 tls 耗时。

日常镜像已再次恢复。LCKFB 按决定未测。改动尚未提交。
