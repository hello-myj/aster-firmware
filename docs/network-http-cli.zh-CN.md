# HTTP/HTTPS CLI 开发记录

状态（2026-10-16）：N2本阶段已完成，两板CLI G1和ABI3.9 APP/SDK验收通过，未发布Release。此前CLI镜像的ABI3.8与新工作区ABI3.9分开记录，见验证报告。

## 构建与范围

在已有板级配置之后追加 `config/diagnostics/network-http.conf`。例如微雪使用：

```sh
west build -d build/network-http-waveshare \
  -b waveshare_esp32s3_touch_lcd_35b/esp32s3/procpu . -- \
  -DEXTRA_CONF_FILE='boards/waveshare_35b/qspi-prefetch.conf;config/diagnostics/network-http.conf'
```

请求在私有8 KiB DRAM栈线程执行。HTTP线程与APP使用同一优先级，每2ms执行时间片后通过定时器让出1ms，避免持续图形负载饿死TLS，也给原有网络线程和shell留出调度机会；未修改全局20ms时间片。TLS按握手步骤推进，每步之间检查取消和总期限；单步密码学计算仍不是可任意中断的API。输入复制到PSRAM，请求体最多2048字节，响应队列2048字节，URL最多1024字节，头部最多2048字节/32字段；响应上限是流量限制，不能理解为一次分配相应大小。CLI受shell命令行长度限制，不能用于直接输入最大长度URL/POST，完整边界由服务层测试覆盖。

HTTP/TCP/UDP共用两槽；重定向复用原槽，更换底层代际句柄，公共请求句柄保持不变。完成/失败/取消的请求仍占名额，调用close后才开始回收。HTTPS最多占一个名额，额度不足返回忙，不抢占其他连接。CLI与APP使用同一个服务。

TLS启用TLS1.2、ECDHE-ECDSA/RSA AES128-GCM套件，证书链、主机名和日期均须通过；系统SNTP尚未同步时拒绝握手。默认根仅ISRG Root X1，**不是完整公共CA集合**。不存在跳过校验选项。默认配置的96 KiB Mbed TLS全局池置于`.mbedtls_heap`的PSRAM区，包验签也使用该池，统计不能称为HTTPS独占用量。PSA/TLS调用与包验签以专用互斥锁串行保护，握手不持HTTP或TCP服务锁。

## 命令

```text
aster net http start GET http://HOST:9080/fixed?n=4096 30000 1048576
aster net http start HEAD http://HOST:9080/fixed?n=4096 30000 1048576
aster net http start POST http://HOST:9080/echo 30000 1048576 hello
aster net http result HANDLE
aster net http read HANDLE 1200
aster net http header HANDLE X-Repeat 0
aster net http header HANDLE X-Repeat 1
aster net http cancel HANDLE
aster net http close HANDLE
aster net http stats
aster net http ca /SD:/aster/n2-ca.pem
aster net http ca default
```

start参数依次为方法、URL、总超时毫秒、响应体流量上限，可选文本请求体及头部文件。没有请求体但需要头部文件时，用 `-` 占位。头部文件必须使用CRLF、每行一个字段，不带末尾空行。Host、Content-Length、Transfer-Encoding、Connection、Accept-Encoding、Expect及Upgrade由服务控制，不允许覆盖。

start返回排队句柄；必须查询result并读取响应，不能把排队成功当成请求成功。state为1连接、2TLS握手、3发送、4等待响应头、5响应体、6完成、7失败、8取消。HTTP 404/500与传输失败分开，status保留HTTP码。read先返回已缓冲数据，再返回0（完成）或内部负errno（失败/取消）；未就绪返回EAGAIN。CLI错误码属于固件内部域，不能写进APP契约。read输出每段十六进制与CRC32，供主机核对。

ca命令仅管理本次开机的HTTPS信任根；不修改APP包签名根。文件为PEM，最大8191字节文本（另加NUL），有HTTPS请求占名额时拒绝更换。私钥永不上传设备。当前PEM解析在HTTPS工作线程中执行，ca排队配置成功不证明证书可解析。

重定向最多3次；跨origin丢弃全部用户请求头，后续不会恢复。拒绝HTTPS降级HTTP。301/302 POST改GET，303除HEAD外改GET，307/308保留方法与有界请求体。响应头值目前只接受ASCII文本；非ASCII字段会被拒绝。不自动解压Content-Encoding，不保存Cookie，不透明重试，不做HTTP2和连接池。

## 可控测试服务器

`tools/validation/network_http_server.cjs`仅依赖Node内置模块，可在Windows原生环境启动，使开发板无需经WSL NAT入站。HTTP端口9080，提供fixed、chunked、eof、echo、headers、redirect、status、truncated、slow和stall路径；数据模式为第i字节`i % 251`。传入仓库外证书目录可启用9443有效证书、9444错误主机名、9445过期证书；9446接受TCP但不握手。

证书目录文件名为server/wrong/expired各自的`.key`与`.pem`。证书必须以测试用CA签署，SAN匹配电脑当前局域网IP或测试域名，设备只安装CA公钥证书。临时防火墙规则只开放本地子网测试端口，结束后删除。证书、私钥、原始日志、镜像和主机抓取均放在仓库外diagnostics目录。

## 已完成与待验收

- 已通过主机ASan/UBSan：逐字节响应、Content-Length/chunked/EOF、临时响应、HEAD/204/304、重复头与trailer、截断/非法帧、重定向方法与跨origin隔离、8 KiB响应与2 KiB背压、取消/总超时、退出回收及共享配额。
- 最新固件103项主机测试全部通过。微雪诊断镜像编译通过；已在实机获得公网HTTP 200和响应内容，以及valid-isrgrootx1.letsencrypt.org的已校验证书HTTPS 200。letsencrypt.org另一个站点在握手中返回底层EPIPE，保留为站点/链路兼容性未解结果，不写成公网全部兼容。
- 微雪首轮21个CLI功能用例通过：包括64 KiB慢读、chunked、EOF、HEAD/POST、404/500、重定向、截断/上限/超时、有效证书、错误主机名/过期/不可信根、故障后恢复、握手及满队列取消。另测HTTPS+UDP共享两槽、第二个TLS拒绝与使用期间更换CA拒绝。完整主机名和过期证书失败有非零验证标志，未绕过校验。
- 首轮本地TLS加密池峰值39868 B；公网附加请求将全局峰值推至51924 B。HTTP对象为约21 KiB PSRAM，底层TCP另占4096 B；TLS上下文和自定义CA另行统计，加密池96 KiB为静态PSRAM保留。首轮HTTP栈峰值3364/8192 B，TCP栈1316/4096 B；版本增加计时与诊断字段后应重测，不能把旧峰值当成最终镜像绝对上限。
- 并发检查发现并修复候选问题：低优先级TLS饥饿、USB定时刷新与写入竞态，以及被长抢占后的虚假发送超时。已有103项主机测试包含强制10000次定时交错与长抢占场景，实体图形并发和最终CLI复测已通过，见下文。
- 微雪图形并发已验证：Gfx Show保持前台，后台HTTPS逐字节校验40800字节，同时完成34轮8字节UDP自环；上拉取消、下拉收回后画面、触摸与动画由用户确认正常。结束后请求缓冲归零，原应用保持运行直到用户反馈。
- 微雪断网恢复已验证：主动断开Wi-Fi，请求在8秒总期限内失败；先读出并校验320字节缓冲，再取得终态错误。保存配置重连成功后，HTTP和HTTPS各4096字节完整校验通过。首次脚本误等待同步连接文字，而CLI实际异步排队；随后通过状态接口确认COMPLETED及operation结果0，再执行恢复请求，不把主机脚本超时写成固件连接失败。
- 微雪最终CLI复测21项通过；局部证书矩阵加密池峰值39868 B，HTTP栈3364/8192 B，网络分片RX14/24、TX4/32，清理后请求缓冲和加密分配归零。具体口径、已知限制及复现见[N2 CLI验证记录](validation/network-n2-cli-2026-10-15.md)。
- LCKFB的21项CLI、TLS/协议池/栈、共享配额和断网恢复自动检查已通过；2048前台并发完整校验64 KiB HTTPS及139轮UDP。实体交互已确认正常，日常固件及原APP已恢复，详细数值见上述验证记录。
- CLI G1通过后已实施公共ABI3.9、SDK、模拟器和获取/下载示例；两板APP层测试已通过，N2本阶段完成，详细结果见[APP/SDK验收](validation/network-n2-sdk-2026-10-15.md)。

2026-10-16 正式配置修正：`MBEDTLS_SSL_MAX_CONTENT_LEN` 在启用 HTTPS 时默认16384，避免日常固件继承Wi-Fi模块的1500字节默认值而与已验证的TLS配置不一致。微雪直连incompetech HTTPS元数据HEAD返回200、verify=0；本机PC的VPN可达性不作为设备网络验收。
