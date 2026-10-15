# N1 APP/SDK 验证记录（已完成，2026-10-15）

结论：ABI3.8工作区开发版已实现七个网络导入、net权限、会话退出清理、SDK契约及示例、Windows/Linux模拟器真实socket支持。两板最终七接口版本均已完成APP检查；用户已确认两板网络与图形并发交互正常。两板系统协议池并发验证已完成并修复分片池不足，N1已完成；见[协议池补测](network-n1-pools-2026-10-15.md)。HTTP/TLS/MQTT未实现。首轮实现已本地提交（固件1a74791、SDK24cd0d8、模拟器8d64b4c）；协议池补测与修复纳入后续本地版本记录，未推送或发布新Release，已有3.7发布包不含新导入。

## 契约与资源

- 七个导入：`aster_network_get_info/open/send/recv/status/cancel/close`。feature `network` 位15、permission `net` 位5；不隐式授予wifi配置权限。
- `aster_network_info_t`为44字节，status为56字节，packet为32字节；WASM32编译器检查3.7冻结基线到3.8候选未发现旧布局、原型或错误值破坏。新硬导入要求abi_min至少3.8。
- 通过固件export_contract、SDK import_contract同步头文件、指纹及CPU目标；没有手工维护第二份ABI，也不要求APP开发者安装固件/ESP-IDF环境。
- native边界先检查全部输入输出及重叠范围，再消费数据。不使用会设置WAMR异常的越界校验路径，非法参数返回稳定EINVAL，实际AOT检查未杀死应用。
- owner是单调uint64会话号，CLI保留1；不能由APP指定。退出/初始化失败/trap在销毁WAMR前请求owner-close，忙时让出线程重试；网络工作者只持有数字owner、复制后的域名/数据。
- get_info返回默认接口的只读IPv4/原始状态位及限额，不创建工作线程或socket；接口锁忙返回EAGAIN。ESP32 carrier位可能在未关联时保持，不将它当作联网证明；模拟器返回宿主IPv4提示，也不保证等于目标路由出口。
- 两个槽与CLI共用。TCP私有PSRAM4096字节、UDP6568字节，二者同时持有时10664字节；后台栈4096字节常驻。终态、关闭中及底层TCP TIME_WAIT不等同于立即空闲的系统协议资源。
- 内部errno经显式映射为ASTER错误；DNS失败使用明确私有标记后映射EDNS，避免把平台EHOSTUNREACH与DNS混淆。CLI仍输出诊断errno，不是APP错误域。

## 自动化和模拟器

- 固件100项测试通过，包括实际服务和native桥接的ASan/UBSan编译测试，跨会话/CLI句柄、输出越界、溢出长度、重叠缓冲、短结构、取消、旧会话、退出锁忙重试等。
- SDK103项测试通过。旧发布迁移测试改为从当前API读取ABI，避免测试拼接出API3.8/lock3.7的无效包；保留其他既有修改。
- 模拟器70项检查：59通过、11项可选外部资源测试跳过、0失败。真实TCP/UDP单测覆盖配额、源地址/内容、空包、短缓冲、丢弃计数、EOF、权限、关闭中DNS配额和陈旧回调。stop断开Host引用，后台收尾不继续持有guest内存。DNS使用独立dns.Resolver.resolve4，取消调用Resolver.cancel，测试验证收尾期间保留配额和陈旧回调隔离。
- 两板日常构建通过。LCKFB实际刷入镜像SHA256：`2469af21af6410db0126876ccbcc59c63fe7ac125c5832bb9df1e514cccdc827`；纯立创配置，故障注入及微雪选项关闭。
- 同一SDK默认Echo包在Linux和Windows原生Node模拟器均成功：TCP512、UDP1200、短缓冲保留/重读、空包，最后PASS并以0退出，Windows最终七接口包为5帧/5次发布；Linux最终包在下面的故障注入测试中复核。
- Linux模拟器另注入首个1200字节包不回送、一次旧轮次回包：真实WASM完成48帧后PASS，报告retries=1/stale=1。序号仅用于二进制包；空包本身不能充当唯一消息ACK。
- `--network`显式启用宿主实际IPv4通信，`--wifi`仍为无线状态模拟。确定性replay禁止真实网络；模拟器不冒充ESP32无线质量或硬件堆预算。源码契约快照标记working tree，未发布成品。

## LCKFB APP 实测

设备先确认ABI3.8、保存热点关联及172.17.35.x地址，电脑使用Windows原生Node服务器；WSL只控制一个串口。均用独立SDK编译的Xtensa AOT包，不从固件源码编译guest。

- 边界fixture mode0：首轮14项检查、二次启动仍14项且全部通过；CLI句柄1保持有效，APP旧句柄2、外国句柄不可访问。APP自己未显式清理，自动退出后仅保留CLI槽；CLI也不能查询APP句柄3。
- mode1首次六接口版无net权限：六个公开导入全部EPERM；最终七接口版补测见下。仅有net权限时wifi_status仍EPERM。
- mode2初始化返回错误前创建两个UDP槽，按既有生命周期策略记为CRASHED/ESTATE；mode3持有UDP后刻意trap，记录CRASHED/ECORRUPT。两者均回到active=0/closing=0/psram=0。测试驱动最初把init拒绝误期望为STOPPED，已据实际既有策略更正记录，不把该断言失败当作固件故障。
- mode4通过APP域名连接example.com:80，发送原始请求，收到888字节且HTTP前缀正确；这验证DNS与原始TCP，不代表提供HTTP客户端。
- mode5实际APP同一回调内TCP依次入队1500、548字节，第三次返回EAGAIN；取消后状态/接收返回ECANCELLED，退出后缓冲归还。7项检查通过。该项验证部分接受/背压，不将尚未发出的2048字节算作对端收到的数据。
- 正常Echo：TCP512、UDP1200、容量1199时保留再读、空数据报均按内容验证；APP主动close并退出，日志明确PASS。不能只用STOPPED(err=0)判断测试成功。
- 双连接持有时，CLI第三个open返回ENOSPC；服务私有PSRAM10664字节，符合配额。

## LCKFB 图形并发及发现

第一次持续版在100轮后遇到UDP等待超时，15秒应用截止后自行退出，所以用户看到主页；这次不计交互通过。未出现系统崩溃或资源泄漏。Echo示例已加入每阶段最多3次重试、1500ms等待和二进制轮次号，迟到回包不能当作新消息。

重新启动后约123秒内至少完成550轮UDP1200字节及空包校验，TCP512字节在开场完成一次；日志累计10次应用层超时重试，服务RX队列丢弃1包。用户上下拉后回复“现在正常”。不把该轮写成零丢包、双协议持续吞吐基准或长时压力验收，也不把10次应用超时换算成无线链路丢包率。

停止后网络active=0/closing=0/psram=0，栈保持4096；全部检查累计opened=14/closed=14、cancelled=1、dropped=1。最终内核size98304/used15352/free82720/peak35660；libc usable123044/used68308/free54736/peak72432。PSRAM最大可分配块探测下界至少5MiB，不是精确总空闲量。

7个临时APP及SD根目录的测试包均已清理，原17个APP保留；旧2048启动、RUNNING、停止检查通过，最后恢复主页、保留Wi-Fi和原音量。没有提交原始日志、图片、镜像或测试凭据。

## 微雪最终七接口实测

- 镜像SHA256：`4fc4c1e5c7eeee564283dfaf67d61a92f1c49678e3c93a0d3cc3ec88389acb5c`。保留RGB565、40MHz、双8KiB内部RAM预拷贝、关闭PSRAM直接DMA；未改变显示驱动/设备树。
- mode0首轮及重开均18项通过，get_info返回172.17.35.210及flags=15；CLI/外国/旧APP句柄隔离成立。mode1七项权限拒绝通过，mode2初始化拒绝前16项通过，mode3故意trap前15项通过；每次退出均active=0/closing=0/psram=0。
- mode4实际DNS、原始TCP收到888字节且HTTP前缀正确。mode5首次因电脑测试服务器十分钟自动退出而连接超时；重启服务器后7项部分入队/背压/取消检查全部通过。累计failed=1保留为这次对端不可用记录，没有隐去。
- Network Echo持续版完成至少500轮UDP1200字节和空包内容校验，开场TCP512字节校验成功；日志可见3次应用层重试，服务dropped=0。用户确认“正常”：标题/动画、上拉取消、下拉打开收回和触摸恢复正常。不是持续TCP吞吐或长时验收。
- 运行时服务私有PSRAM10664字节；内核堆size98304/used32576/free65496/peak41764，libc usable124664/used64184/free60480/peak68308。停止清理后内核used15276/free82804，网络opened=11/closed=11、active=0/closing=0/psram=0，后台栈4096字节保留。PSRAM最大可分配块下界至少5MiB。
- 临时7个APP及根目录AST已删除；旧Gfx Show AOT可启动并停止，原13个应用保留，最后主页、原Wi-Fi与音量保留。
- 刷入后曾出现一次SD未挂载、apps=0；软件冷重启后SD挂载恢复且原应用完整。本轮未修改SD驱动，不把此现象认定为已修复。

## 最终 LCKFB 七接口补测

最终镜像SHA256：`882287bfe98d0cac25847aafb34400fcd6af58b2977599197ee9160b5a4795cd`。纯LCKFB日常配置，网络上下文8、DNS并发3、DNS服务器上限1、内核堆96KiB。

- mode0两次各18项通过，get_info返回172.17.35.157/flags15；mode1七项权限拒绝通过；mode2初始化拒绝前16项、mode3刻意trap前15项均零失败，自动清理后连接及缓冲为0。
- mode4真实DNS/TCP收到888字节且HTTP前缀正确；mode5的7项部分入队/背压/取消通过。最终有限次Echo明确记录PASS rounds=1/retries=0/stale=0，校验TCP512、UDP1200、短缓冲保留再读及空包。
- 补测脚本曾因复制目录中保留的旧持续测试AST而先启动持续版：运行至少250轮后主动停止。随后使用实际新构建产物重新部署有限次版并取得上述PASS；不把错误副本当作最终版本验证。
- 累计opened=12/closed=12、failed=0/cancelled=1/dropped=0，最终active=0/closing=0/psram=0；内核used15316/free82756/peak35660，libc used68308/free54736/peak72432。后台栈保留4096字节；PSRAM最大块探测下界至少5MiB。
- 临时7个APP及根目录AST已删除，原17个APP保留；原2048启停正常，恢复主页、保留原网络和音量。串口已释放。


## 完成边界与后续验收

- 两板最终APP桥接结果已写入本记录，公共接口及G2边界项已关闭；系统DHCP/DNS/SNTP与双连接的协议池占用已完成两板后台采样与分配高水位检查，不能把服务自己的active=2当成全系统仅2个socket。
- 当前应用/CLI共享两槽；系统总上下文8，DNS并发3并不等于固定3个socket；DNS服务器上限1。DHCP客户端使用平台内部路径，SNTP及个人热点DHCP服务另有开销，TCP关闭后也可能保留协议状态。两板补测均通过，分片池已据实际分配失败调整为256字节RX24/TX32，阶段资源项关闭。
- 完整长时、TLS、HTTP、MQTT不在本轮通过范围；HTTP/TLS/MQTT仍属N2/N3，N4统一验收尚未执行。不以本次通过推断所有启动重连异常均已修复。

复现入口：固件`python -m unittest discover -s tests`；SDK同名测试命令；模拟器`node --test test/*.test.js`及`node scripts/check-sdk.js ../aster-sdk`。设备fixture在SDK `tests/fixtures/network_probe.c`，配置和构建产物由测试驱动写入仓库外。APP示例与独立Node服务在SDK `apps/network-echo/`、`tools/network_echo_server.cjs`，完整开发手册已包含全部公开声明及事件驱动源码。原始串口日志、镜像和截图只保存在仓库外diagnostics目录。
