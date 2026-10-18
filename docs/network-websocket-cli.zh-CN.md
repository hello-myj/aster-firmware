# N5.1 WebSocket/WSS 内部服务与 CLI（实施中）

基线为N4本地提交固件`8a71fd2`、SDK`cbbd5ec`、模拟器`5e34c85`。双板G1与G2公共链路已实施并本地提交于`feature/network-n5`（未推送）：公共ABI选定3.12.0，SDK已提供六个`aster_ws_*`导入、`apps/ws-echo`示例与模拟器实现；均未发布Release，N5.2–N5.5未实施，N6接口统一尚未开始，模拟器Windows/Linux各95项零跳过已复跑，实机取消/trap专项已双板执行并记录一项cancel可见性待评审偏差；阶段完成仍需逐项复核，不表示N5.1已完成。详见[双板G1记录](validation/network-n5-ws-cli-2026-10-17-lckfb-g1.md)、[双板APP G2记录](validation/network-n5-ws-app-g2-2026-10-18.md)与[Cursor实施交接](network-n5-cursor-handoff.zh-CN.md)。

## 固定范围及资源

- 一个WebSocket客户端，与HTTP/HTTPS、MQTT/MQTTS、TCP/UDP/CLI共用两个传输名额；WSS与HTTPS/MQTTS共用一个TLS名额及同一默认/管理员信任源，不允许关闭证书校验。
- 使用现有HTTP/MQTT worker，不增加线程或私有TLS池。客户端及其收发缓冲在open时申请PSRAM，close后台收尾后释放；仅运行普通APP且没有打开WS时不申请客户端缓存。worker栈沿用既有按需常驻策略，不能把常驻栈误判成泄漏。
- 单条文本/二进制消息最多4096字节，发送一帧，底层允许部分写；接收支持分片及穿插控制帧，总重组上限4096。一个完整消息待取，另有有界重组/输入缓存。禁止根据远端64位长度申请内存；过大长度在帧头阶段拒绝。
- 客户端握手nonce和每帧mask均来自`sys_csrand_get`，随机源失败直接报错，不回退固定mask/伪随机测试源。Zephyr选择硬件CSPRNG；主机fixture使用可注入随机源，仅验证错误处理。
- 初版仅ws/wss、IPv4/DNS、无用户信息的URL；不支持扩展、压缩、子协议、自定义鉴权头、重定向或自动重连。URL复用HTTP解析器，转换前后都受其1024字节上限约束（ws/wss原始URL有效上限1022字节），路径可含query，禁止fragment。
- connect总期限100..30000ms（默认15000），覆盖DNS、TCP、TLS及升级；待发帧总期限10秒，分片重组期限30秒。空闲15秒发送ping、10秒内等待匹配pong。慢消费造成的背压不能无限延长期限，错误后须close再按业务规则重新连接，不盲目重发业务消息。
- close立即使句柄失效；已建立的连接给关闭帧及对端确认最多1500ms，然后强制释放。关闭帧不能插入尚未完成的帧字节中。cancel保留句柄/终态，close才释放名额；已完成消息先于终态错误交付。协议错误可直接中止，不承诺对损坏链路成功完成关闭握手。

协议依据：[RFC6455](https://www.rfc-editor.org/rfc/rfc6455)。使用Aster已有TCP/TLS后端，协议解析独立有界，不另建socket/TLS栈。

## CLI

仅`CONFIG_ASTER_WEBSOCKET=y`时提供；默认关闭，诊断配置为`config/diagnostics/network-websocket.conf`。

```text
aster net ws open ws://192.168.1.10:17880/echo 15000
aster net ws status HANDLE
aster net ws send HANDLE 1 48656c6c6f
aster net ws send HANDLE 2 -
aster net ws pattern HANDLE 4096 17
aster net ws recv HANDLE 4096
aster net ws cancel HANDLE
aster net ws close HANDLE
aster net ws stats
```

`open`返回正句柄仅表示提交，state=4才为READY。send返回0表示有界入队，不是远端业务确认；忙时EAGAIN留待后续调用。opcode 1文本/2二进制；`-`为空消息。pattern仅为CLI边界测试生成二进制数据`(offset+seed)%251`，不需要发送8KiB十六进制命令。

recv返回0表示一条完整消息，包含合法空消息；`-EAGAIN`表示暂无消息，`-ENODATA`表示正常关闭且缓存已取完。容量不足返回`-EMSGSIZE`并报告required及opcode，消息保留。CLI显式recv才输出payload与CRC，状态/统计不打印消息和认证数据。

内部state：CONNECTING=1、TLS=2、UPGRADE=3、READY=4、CLOSING=5、CLOSED=6、FAILED=7、CANCELLED=8。当前CLI使用私有errno，不能把它们当未来APP ABI错误域。句柄来自共享传输代次；协议服务以owner隔离，外部不能用普通TCP接口读写WS私有传输。

## 验证及状态

- 新增RFC示例nonce/accept向量、升级头、0..4096长度及每个截断边界、mask、非规范长度、UTF-8和关闭码检查，ASan/UBSan执行真实C代码。
- 服务fixture用真实POSIX TCP、实际网络/HTTP/WS服务及手动worker；检查分片中文/控制帧、短缓冲保留、空消息、4KiB实际masked发送、慢取消息、非法帧拒绝、随机源失败及100轮取消/关闭回收。TLS fixture验证共享准入与失败收尾，不替代实机证书/加密验证。
- 固件主机检查现123项全部通过（含新增WS桥接fixture，覆盖权限门、guest边界与复制、所有权、终态先交付、预算耗尽与回收）。G1：微雪与LCKFB各自通过15类局域网CLI、断网恢复、WSS/HTTP图形并发与实体反馈及13项补充边界门禁，双板WS/WSS私有PSRAM样本一致（18040/19887B）；详见[CLI验证记录](validation/network-n5-ws-cli-2026-10-17.md)与[双板G1补充记录](validation/network-n5-ws-cli-2026-10-17-lckfb-g1.md)。G2：双板真实AOT包APP门槛已通过（明文+WSS回环内容、5项边界探针、CLI与APP预算争用、停APP回收、实体反馈），见[双板APP G2记录](validation/network-n5-ws-app-g2-2026-10-18.md)；模拟器Windows/Linux各95项零跳过复跑已完成（Windows原生Node v24.14.0、Linux Node v20.18.0，含真实SDK ws-echo包与HTTPS/MQTTS/WSS证书夹具），双平台不构成缺口；实机取消/trap专项与逐项复核完成前，不能宣称N5.1整体完成。
- 测试对端`tools/validation/network_ws_peer.cjs`使用已有Node ws依赖，原生Windows Node服务端口17880–17885；CA/私钥及日志只留仓库外。需要临时Windows Private/LocalSubnet规则，结束后停止对端并删除规则；恢复原应用/音量/网络和日常固件、移除SD测试CA。
