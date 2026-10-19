# WebSocket/WSS

全设备一个 WebSocket 客户端，与 HTTP、MQTT、TCP/UDP 共用两个连接槽。WSS 与 HTTPS、MQTTS 共用一个 TLS 名额。诊断配置是 `config/diagnostics/network-websocket.conf`。

单条消息最多 4096 字节。URL 为 `ws://` 或 `wss://`，最长 1022 字节，不带用户信息、fragment、子协议或自定义头。不自动重连。

```text
aster net ws open ws://<主机>:17880/echo 15000
aster net ws status <句柄>
aster net ws send <句柄> 1 48656c6c6f
aster net ws send <句柄> 2 -
aster net ws recv <句柄> 4096
aster net ws cancel <句柄>
aster net ws close <句柄>
aster net ws stats
```

`open` 返回句柄只表示已提交，state 为 4 才是 READY。opcode 1 是文本，2 是二进制，`-` 是空消息。`send` 返回 0 表示已进入发送队列。

state：1 连接、2 TLS、3 升级、4 READY、5 关闭中、6 已关闭、7 失败、8 已取消。`recv` 返回 0 表示拿到一条完整消息；容量不够时报告所需长度并保留该消息。

连接超时 100..30000 ms，默认 15000。空闲 15 秒发 ping，10 秒内等待 pong。`close` 使句柄失效；`cancel` 保留终态，要再 `close` 才释放名额。

测试对端：

```bash
node tools/validation/network_ws_peer.cjs
```
