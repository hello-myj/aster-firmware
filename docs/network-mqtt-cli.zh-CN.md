# MQTT/MQTTS

实现 MQTT 3.1.1，支持 QoS 0/1、retain、用户名、密码、遗嘱和 TLS。会话为 clean session。与 TCP/UDP/HTTP 共用两个连接槽；MQTTS 与 HTTPS/WSS 共用一个 TLS 名额。

每个客户端最多 4 个主题过滤器、2 条接收消息、2 个排队操作。主题 128 字节，payload 1024 字节。

```text
aster net mqtt connect /SD:/mqtt.profile
aster net mqtt result <句柄>
aster net mqtt subscribe <句柄> aster/demo/# 1
aster net mqtt publish <句柄> aster/demo/state 1 0 68656c6c6f
aster net mqtt operation <句柄> <序号>
aster net mqtt recv <句柄>
aster net mqtt unsubscribe <句柄> aster/demo/#
aster net mqtt cancel <句柄>
aster net mqtt close <句柄>
aster net mqtt stats
```

`publish` 参数是 `句柄 主题 QoS RETAIN HEX`。shell 一次最多 96 字节；`-` 表示空 payload。

profile 是不超过 2048 字节的 `key=value` 文本：

```ini
host=192.0.2.10
client_id=aster-example
port=1883
tls=0
keepalive_s=15
timeout_ms=15000
reconnect=1
```

可选键：`username`、`password`、`will_topic`、`will_hex`、`will_qos`、`will_retain`。不要把真实密码写进可提交的文件。CA 仍用 `aster net http ca`。

keepalive 为 5..300 秒，连接超时 100..30000 毫秒。可选重连间隔为 1、2、4、8、16、30 秒。认证失败、证书失败和非法报文不会自动重试。

测试对端：

```bash
node tools/validation/network_mqtt_broker.cjs
```
