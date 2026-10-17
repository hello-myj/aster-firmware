# N5.1 WebSocket/WSS CLI 验证（进行中）

基线：N4固件8a71fd2、SDK cbbd5ec、模拟器5e34c85；本轮按用户要求提交阶段检查点，公共ABI仍3.11。先完成两板CLI G1，再封装SDK/APP G2及模拟器。N5.1和N5整体均未完成。

## 主机与构建

固件122项主机检查通过，包含真实C协议及服务ASan/UBSan：RFC nonce/accept向量、0..4096长度所有截断边界、客户端mask、非法长度/UTF-8、分片中文及穿插控制帧、短缓冲保留、空消息、4KiB真实socket发送、消息背压、100轮取消/关闭、随机源失败及TLS配额/错误收尾。主机TLS fixture仅验证共享准入及错误传播，不替代密码学/证书验证。

微雪诊断配置构建并刷入，纯微雪、RGB565/40MHz/双8KiB、96KiB内核堆保持；增加的WS使用既有HTTP/MQTT worker。默认正式配置未开启WS，未增加公开APP导入。

## 微雪基础G1

- 实际LAN为开发板192.168.110.92、电脑192.168.110.201；Windows原生Node本地对端，临时Private/LocalSubnet TCP17880–17885规则已核实启用。无公网/VPN路径依赖。
- 第一轮WS明文通过，WSS状态查询遇到预期EAGAIN，验证脚本误判失败且关闭请求也未重试。已补有界重试与清理结果校验，回收旧测试句柄、恢复默认根后从头运行；失败原始记录保留，不作为通过结果。
- 第三轮完整15类CLI检查通过：WS/WSS文本、二进制、空消息和4096字节回环；短缓冲报告长度且保留消息；WSS+HTTP16384字节内容校验与共享传输/TLS名额限制；分片中文与ping穿插；错误Accept、超长帧、非法UTF-8、服务器masked帧拒绝；错误名称及过期证书拒绝；TCP/TLS握手停滞超时；服务端关闭码1000；空闲ping/pong；升级期间取消。两类证书错误验证标志非零，实际使用共享验证TLS后端，未跳过验证。
- 每次关闭后WS active/closing/tls/psram归零。结束NET opened=closed=18，active/closing/psram归零；默认根恢复，上传CA删除。

## 微雪采样资源

- 明文WS服务私有PSRAM18040B；WSS服务19887B（包含551B测试CA及1296B TLS上下文），另占共享TCP槽4096B。HTTPS/HTTP/MQTT都共用原有两传输/一TLS预算。
- WSS+HTTP顺序状态样本：WS19887B、HTTP23312B，两个底层TCP槽共8192B；TLS内部使用33536B、全局高水位39824B属于静态96KiB保留，不能重复叠加为额外PSRAM。各命令非原子快照。
- 暖机前后libc均64184B，采样峰值68308B；内核堆15276→15312B。常驻HTTP8192B/网络4096B栈沿用按需策略；没有新WS线程。栈水位HTTP3044/8192B、网络1316/4096B。
- 系统上下文观察峰值4/8为本次启动全局采样下界，不能当分配钩子绝对峰值，也不能证明小泄漏不存在。

## 微雪恢复与图形并发

- 断网后旧WSS连接转失败并关闭，使用保存的网络重连，新WSS连接真实内容校验通过。
- Gfx Show与WSS真实消息校验、HTTP第二名额同时运行时，用户确认“画面、触摸和上下拉恢复都正常”。
- 第一轮并发脚本将16KiB HTTP响应留到人工反馈后才读取，响应队列背压期间超过600秒请求总期限，最终读取返回-116。因此该轮自动并发门禁不记通过；WSS与实体反馈结果单独保留。收尾WS/HTTP/NET active、closing、PSRAM均归零，NET opened=closed=22，默认CA恢复、测试CA删除。
- 已将脚本改为在WSS回环期间持续消费HTTP响应，逐个验证完整16KiB内容并重新请求；修正后的90秒循环门禁通过：6轮WSS消息回环及1份完整16KiB HTTP响应校验。结束WS/HTTP/NET active、closing、PSRAM均归零，NET opened=closed=24，TLS内部current=0，默认CA恢复、测试CA删除。该轮是有界串口内容校验，不作为网络吞吐测试。

## 交接收尾

交接前固件122项主机测试重跑通过（19.366秒）。微雪已恢复日常镜像并释放串口；Windows测试对端已停止。删除临时规则被Windows拒绝访问，`Aster N5.1 local validation`仍待管理员删除。后续范围、命令和验收见[Cursor交接](../network-n5-cursor-handoff.zh-CN.md)。

## 待完成

- 微雪已恢复日常固件并返回主页，镜像SHA256为6291edaffaca107e8baae63ee6b185ac56211363a6695ddbb350cd8401d94057，与原日常镜像一致；原14个APP均STOPPED、音量53、原Wi-Fi连接保留。LCKFB同套CLI/资源/图形验证仍待接入。
- 恢复每板原APP集合、前台、音量、网络及日常固件，清理CA/上传物；完成双板后停止对端并删除临时规则。
- 完成CLI后再新增SDK/模拟器/远程控制示例，验证APP权限、越界、异常退出及资源回收。

原始日志、JSON、证书和镜像均在仓库外本地diagnostics/network-n5-20261017，不入Git，不发布Release。
