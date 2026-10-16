# MQTT/MQTTS 固件服务与 CLI（N3 已完成）

2026-10-16：两板已通过内部服务/CLI真实收发、证书拒绝、恢复和图形并发交互。并发实体交互曾发现并修正输入起点丢失与优先级问题。公共ABI3.11、SDK与模拟器已接入，APP层两板验收也已通过，设备及主机测试环境清理完成，N3已完成。详见[SDK/APP验收](validation/network-n3-sdk-2026-10-16.md)。进度与失败记录见[CLI验收记录](validation/network-n3-cli-2026-10-16.md)。MQTT 3.1.1依据[OASIS规范](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)实现。N6统一网络ABI仍在N3–N5之后，本阶段不提前改用统一分派入口。

## 资源与执行模型

- 依赖`CONFIG_ASTER_NET`、`CONFIG_ASTER_HTTP`，新增`CONFIG_ASTER_MQTT`；MQTTS同时依赖`CONFIG_ASTER_HTTP_TLS`。复用HTTP的8 KiB工作栈、CPU预算和已有TCP worker，不为MQTT增加独立常驻栈。
- MQTT和TCP/UDP/HTTP/CLI共用两个传输名额。每个MQTT实例独占一个名额，终态和退避期间仍占用，close/owner退出后由worker回收；不建立隐藏socket。
- MQTTS和HTTPS共用一个TLS名额；准入时保留，重连和未关闭终态仍占用。额度不足返回忙，不抢断已有连接。CA来自已有管理员配置，MQTT持有一份有界快照；租约期间禁止替换CA。
- 每客户端：最多4个主题过滤器、2条接收消息、2个排队/待确认操作。主题128字节、payload1024字节；单报文缓冲1536字节。所有会话数据在PSRAM按需分配，关闭后回收；统计包含CA副本与TLS对象，TLS内部堆另看HTTP的全局crypto统计。
- 第一条网络请求会初始化共享worker栈；该栈之后保留，不能把服务私有PSRAM归零表述为全部网络内存归零。实际DRAM、PSRAM与协议池峰值须由两板补测，不以估算代替。

## 协议边界

- 支持MQTT 3.1.1、QoS0/1、retain、用户名/密码、遗嘱、TLS证书链/名称/时间验证。初版仅`clean_session=true`，不提供持久会话、QoS2、MQTT5、WebSocket传输或设备端broker。
- QoS0完成表示已交给有界传输服务；QoS1完成表示收到匹配PUBACK。两者均不等于远端业务已执行。收到的QoS1只有在完整复制进接收队列后才ACK，APP消费之后不另发业务确认；重复消息按正常消息交付，APP自行幂等。
- publish/subscribe返回正操作序号，仅表示操作接受。`operation`查询最后4个完成记录，EAGAIN表示仍待处理，ENOENT表示未知或已淘汰；APP需在继续提交足以淘汰记录的任务前读取结果。status里的last_operation只是最近一次用户操作完成信息，不能替代逐项结果。
- 接收保留消息边界，包括零长度和二进制payload。接收队列满时停止消费完整报文，不覆盖旧数据；有界TCP背压期间仍推进输出/心跳。长时间不消费可能阻挡PINGRESP并导致超时重连，不承诺无限慢消费者永远保持连接。
- worker独立维护keepalive，APP不轮询也继续心跳。初始keepalive5–300秒；连接总超时100–30000毫秒，操作确认超时10秒。
- 可选自动重连使用1、2、4、8、16、30秒有界退避，网络失败会重连；认证拒绝、证书失败、非法协议与超限报文终止，不做无限认证重试。已确认订阅在重连后重新订阅。
- 断线时排队和待确认发布明确失败，不建立离线积压、不自动重发业务发布；即使未收到PUBACK，broker也可能已经接收。APP选择重新发布时可能重复。
- close立即使句柄无效。正常已连接情况下先完成部分发送帧，再发DISCONNECT，并给传输最多1.5秒排空；链路不可用或排空超时时只能中止，broker仍可能发布遗嘱。cancel不发DISCONNECT，保留终态直到close；遗嘱验证需使用正常关闭和异常断线分别对照。
- 字符串使用有界UTF-8；不接受NUL、控制字符、代理区和非字符。主题名禁止通配符，过滤器仅允许合法完整层级`+`和末尾`#`。payload保持原始二进制。

## CLI

所有调用提交到同一个固件服务，不在shell里等待DNS、TLS或broker应答。CLI句柄owner为1，APP接口随后按session owner封装。

```text
aster net mqtt connect /SD:/n3-plain.profile
aster net mqtt result HANDLE
aster net mqtt subscribe HANDLE aster/demo/# 1
aster net mqtt publish HANDLE aster/demo/state 1 0 68656c6c6f
aster net mqtt operation HANDLE SEQUENCE
aster net mqtt recv HANDLE
aster net mqtt unsubscribe HANDLE aster/demo/#
aster net mqtt cancel HANDLE
aster net mqtt close HANDLE
aster net mqtt stats
```

publish参数为`HANDLE TOPIC QOS RETAIN HEX`，`-`表示空payload，shell单次发送上限96字节；不是底层1024字节能力上限。接收支持完整1024字节，按64字节分行hex并附CRC32供主机核对。结果统一`MQTT`前缀；私有errno不直接作为APP ABI错误码。

profile为不超过2048字节的UTF-8 `key=value`文本，每行一项，不允许重复或未知键；`#`开头行是注释。示例不含真实凭据：

```ini
host=192.0.2.10
client_id=aster-example
port=1883
tls=0
keepalive_s=15
timeout_ms=15000
reconnect=1
```

可选键：`username`、`password`（均最多128字节）、`will_topic`、`will_hex`（最多256字节payload）、`will_qos`、`will_retain`。client_id最多64字节。TLS端口由profile指定；不要把真实密码放CLI参数或可提交文件。该profile是管理员测试文件，不是已实现的硬件加密凭据存储；完成后删除。APP未来通过有界结构复制提交，不能直接读管理员文件。

CA配置复用现有命令：

```text
aster net http ca /SD:/n3-ca.pem
aster net http ca default
aster net http stats
aster net stats
```

## 主机与实机验证

`tests/test_mqtt_service.py`编译真实MQTT、HTTP和TCP服务，在POSIX loopback上手动推进worker；ASan/UBSan覆盖严格报文解析、分片、二进制/空消息、QoS确认、重复交付、慢消费、订阅拒绝、心跳、重连重订阅、错误与取消、owner隔离和100轮生命周期。TLS配额测试使用mock crypto，只能证明租约与回收，不能证明真实握手或证书校验。

独立broker使用`tools/validation/network_mqtt_broker.cjs`，测试依赖Aedes在仓库外安装；不是固件/APP运行时依赖。有效、错域名、过期证书及私钥在本地生成，不提交Git。MQTT1883、MQTTS8883/8884/8885、HTTP9080；1884/8886为只接受TCP但不应答的超时/取消测试端口；控制端口9183仅绑定broker主机loopback。WSL验证时可在Windows运行broker，开发板走真实LAN，不经过电脑VPN；控制请求由Windows原生Node发到本机loopback。

`tools/validation/network_mqtt_capture.py`执行CLI基础门槛，包括独立收发、最大payload、keepalive、认证/订阅拒绝、重连订阅恢复、遗嘱、真实TLS及共享名额拒绝。该脚本不替代Wi-Fi断电/断网、broker进程重启、长时并发、两板APP生命周期和SDK/模拟器验收。

完成CLI G1后才导出公共契约、接入SDK/native桥接、APP退出清理与模拟器，再开发仪表盘示例。最终必须恢复原前台、音量和用户网络配置，删除临时profile与CA，恢复默认根证书，并记录源码侧验收结果。
