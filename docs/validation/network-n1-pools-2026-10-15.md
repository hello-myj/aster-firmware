# N1 系统协议池并发验证（已完成，2026-10-15）

两板三种双连接组合及系统DHCP/DNS/SNTP并发均已通过，N1剩余资源预算项关闭。此次包含一处实际配置修复：旧发送分片池不足以支持较大TCP数据与UDP并发。完成范围为本轮N1受控负载，仍保留文末列出的后续阶段与已知限制。

## 原配置故障及修复

旧配置为RX/TX各8个net_pkt、RX/TX各12个128字节数据分片。此前TCP512字节与UDP1200字节首轮检查通过，不代表TCP更大数据也有足够缓冲。

LCKFB实测TCP1200字节与UDP1200字节同时工作，APP首轮出现ENOMEM；原生net mem显示TX DATA高水位12/12、TX包分配失败10次，平均失败等待约109073微秒。TCP需要保留待确认数据及发送副本，不能只按应用提交的单份payload估算底层池。该次失败发生在DHCP并发测试前，不归因于Wi-Fi重新取址。

两板改为数据分片256字节，RX24片、TX32片；包描述符仍RX8/TX8，上下文8、连接处理项8、DNS并发3、DNS服务器上限1、内核堆96KiB均保持。没有无限扩大队列或增加APP的两连接额度。

ELF符号证实网络数据及net_buf数组位于0x3c开头的PSRAM：新数据区RX6144/TX8192字节，net_buf数组RX672/TX896字节，总计15904字节；旧配置3744字节，增加12160字节（11.875KiB）。该增量不挤占DRAM动态堆；不能将这个共享固定池与每APP的私有TCP/UDP缓冲混为一谈。两板日常ELF地址及大小均已核对。

## 统计口径

- config/diagnostics/network-pools.conf仅供诊断，开启原生net shell、net_buf高水位、net_pkt分配成功/失败统计，并开启独立2KiB栈的netpool采样器。日常镜像关闭所有这些选项。
- netpool通过公开net_context_foreach遍历，名义每5ms采样。观察到NTP端口123并且服务active=2时，另外计数。采样期间APP保持两条真实连接；计数关联并非原子全系统快照，也不是分配钩子保证的绝对瞬时峰值。
- net conn串口快照较慢，可能看不到短暂SNTP连接；netpool确实捕捉到该重叠。net mem的MaxUsed来自分配器，自启动累计，不随netpool reset清零；多场景后报告的是本次启动累计高水位。
- Zephyr DHCP客户端不创建普通net_context，而是常驻注册UDP68连接处理项。当前STA模式下，DNS一个上下文、APP两个、SNTP临时一个，资源模型为4个上下文、至多5个对应连接处理项（含DHCP）。串口处理项表观察值与源码上界分别记录，不能把推导上界说成直接采样峰值。
- DNS并发3个查询不等于固定3个socket；目前服务器上限1，查询复用其socket。个人热点DHCP服务会额外创建socket，未在本轮开启STA/AP并发；未来监听/发现/TLS也不能绕过系统总预算。TCP TIME_WAIT仍可能占协议对象，高频重开需处理资源不足，而不是承诺无限立即重建。
- 使用真实DNS查询（随机example.com子域的NXDOMAIN是预期结果）、DHCP客户端重新取址及固件实际SNTP服务。校时成功日志与APP内容校验互相印证。DNS统计drop含否定应答，不能将其直接记为缓冲耗尽或无线丢包率。

## LCKFB

诊断镜像SHA256：152f57ee6d2e80d36ff9ce5c897dde56bb2915dea6534babe93ac82e88de9b88。

每种连接组合均采集约25秒，并在双连接保持时重启DHCP、发起DNS查询、触发实际SNTP校时。APP为独立SDK编译的Xtensa AOT fixture，TCP每轮2048字节、UDP每轮1200字节，逐字节核验。

- TCP+UDP：观察到上下文4/8，11次APP双连接与NTP重叠采样；日志至少TCP400轮/UDP486轮内容通过。
- TCP+TCP：观察到上下文4/8，33次重叠采样；两路均至少200轮通过。
- UDP+UDP：观察到上下文4/8，14次重叠采样；两路均至少300轮通过，应用层合计5次有界重试，stale=0。
- 双UDP的首版测试fixture未重试，在重新取址时遇到UDP回包未达而截止退出；该轮不计通过。补充每阶段1500ms、最多3次的有界重试及轮次关联后，重新完整通过。没有修改固件UDP为可靠协议，也不把重试次数当成物理丢包率。
- 本次启动累计net_pkt RX高水位3/8、TX4/8，数据分片RX17/24、TX24/32；修复后分配统计只有OK、没有FAIL记录。上述是实际分配高水位，分别保留5/4个包描述符和7/8个数据分片余量。
- 串口快照最多观察到3个上下文/4个连接处理项；短暂NTP由后台采样确认上下文4。处理项模型上界5/8，不能声称串口已经直接量到5。
- 每个场景停止后active=0/closing=0/psram=0，最后opened=8/closed=8、failed=0/cancelled=0/dropped=0，常驻网络栈4096字节保留；网络worker栈高水位1316/4096，诊断采样器436/2048。
- 诊断配置内核堆peak35660，双UDP测试后used15352/free82720；libc usable119220/peak72432/used68308/free50912。PSRAM最大可分配块探测至少6MiB，仅是当时下界，不是固定APP可用空间承诺。
- 2个临时应用和根目录AST已清理，原17个APP保留。日常固件已恢复，原2048启动正常。日常镜像SHA256：022f15f553620dea712cc93c5a69b09ea26c0077d3fef34b73d9fa8582044938；net shell/采样器/分配诊断均关闭。
- 本轮仍遇到已知Wi-Fi重启自动重连超时，手动连接恢复后执行测试；该问题未在本次修复，不据协议池通过宣称所有启动联网故障均已消失。

固件100项自动化测试、SDK104项测试通过，两板诊断/日常构建及文档检查通过；三种fixture均由独立SDK生成WASM/Xtensa AOT。没有修改模拟器或公共ABI。

## 微雪

诊断镜像SHA256：13f83e6178300d5e4b0d2469c7d22c6bf58336d19473925b9f8a6ed9da913368。保留RGB565/40MHz/双8KiB内部RAM预拷贝，关闭PSRAM直接DMA及IMU。测试使用同一Windows原生Node监听，TCP保持连接、UDP原样回送；未修改屏幕驱动。

- TCP+UDP：上下文观测最大4/8，15次APP双连接与NTP重叠采样；日志至少TCP183轮/UDP200轮校验通过，应用层重试7次，stale=0。
- TCP+TCP：上下文观测最大4/8，27次重叠采样；两路均至少200轮通过，重试0；原生net conn明确捕捉到4个上下文与5个连接处理项，符合APP2+DNS1+SNTP1及DHCP客户端处理项的资源模型。
- UDP+UDP：首次在net dhcpv4 client start 1后，旧端点内部发送错误映射为ASTER_EINVAL，测试APP退出；池中没有分配FAIL。该轮不计通过，也不能将SNTP已经成功但APP已退出的结果算为并发重叠。
- SDK fixture补充仅针对幂等UDP echo的有界重建：只对接口相关的EINVAL/ENETUNREACH/ECONNRESET终态重建，最多每端点3次，退避250ms并等待本地IPv4，恢复等待最多10秒；ENOMEM、非法guest参数及数据错乱仍立即失败。UDP包重试仍为1500ms、最多3次。不会把这套策略当作任意业务指令的自动重放规则。
- 双UDP重新完整运行约25秒，在取址期间实际发生1次旧UDP端点重建并恢复；两路至少167/200轮通过，应用层重试14次、stale=0。观测上下文4/8，11次双连接与NTP重叠采样。原始失败记录保留在仓库外，不抹掉累计错误计数。
- 本次启动累计net_pkt RX高水位5/8、TX6/8，数据分片RX16/24、TX24/32；分配统计始终没有FAIL记录。分别保留3/2个包描述符及8/8个数据分片余量。
- 最终服务opened=9/closed=9、active=0/closing=0/psram=0；failed=2为首轮退出及重建轮次各一个取址期旧UDP端点错误，cancelled=0/dropped=0，不宣称正常过程零终态错误。后台栈保留4096字节，网络线程栈高水位1316/4096，诊断采样器436/2048。
- 诊断配置内核堆size98304/peak41764，清理后used15276/free82804；libc usable120840/peak68308/used64184/free56656。PSRAM最大可分配块探测至少5MiB，仅为当时下界。
- 临时APP及根目录AST已清理，原13个APP保留。恢复日常镜像86b85b5a8d7b78a16c1afe26c1e17c4a55171ec4c5591bdf19df86ee62d890a2，net shell/采样器/分配诊断均关闭；旧Gfx Show AOT启动、RUNNING与停止检查通过，最后恢复主页，13个APP均停止。此次日常镜像重启自动联网成功，复核IPv4为172.17.35.210，保留原音量与热点配置。新启动尚未创建网络worker，服务stack=0；这与诊断轮中保留4KiB worker的统计口径不同。
- 和LCKFB一样，重启后仍可能自动重连超时；手动连接已授权热点后开展测试。此已知问题不计作缓冲池修复，也不宣称已解决。


## 复现

1. 日常硬件配置不变，使用额外片段构建诊断镜像。例如：west build -d build/network-pools-lckfb . -- -DASTER_USER_CONFIG=/绝对路径/立创配置.conf -DEXTRA_CONF_FILE=/绝对路径/config/diagnostics/network-pools.conf。微雪指定微雪硬件配置；避免复用另一板型的CMake缓存。
2. 独立SDK使用tests/fixtures/network_pool_probe.c，manifest声明net权限、network能力、ABI/min至少3.8；POOL_MODE=0/1/2分别编译三种双连接，NETWORK_HOST为电脑局域网IPv4。TCP17780服务器须原样回送并保持连接，UDP17781原样回送不超过1200字节的数据报；电脑与开发板同网段。
3. APP运行后，调用netpool reset，再运行tools/validation/network_pool_capture.py --sdk-tools ../aster-sdk/tools --port /dev/ttyUSB0 --output 仓库外目录 --seconds 25 --dhcp-restart。该工具独占串口；不要同时运行其他监控或传输进程。
4. 确认APP保持RUNNING且两路内容校验递增，后台采样app2_ntp_samples必须大于0；若APP提前退出或没有实际SNTP重叠，该轮不计通过。查看APP内容校验日志、netpool的app2_ntp_samples/app2_ntp_max、net conn、net mem、net stats、两套堆和线程栈。关闭APP，等待TCP收尾并确认私有缓冲归还。处理应用层UDP有限重试，不把入队成功等同对端收到。
5. 清理测试包，恢复诊断选项关闭的日常固件和原应用；保存生成配置、镜像哈希与原始日志在仓库外diagnostics目录。仓库仅保留配置、fixture、采集工具与精简结论。

本轮为N1资源预算验证，不替代N4长时复合压力、TLS峰值、个人热点并发或未来HTTP/MQTT验收。当前公共ABI仍为3.8，未发布新Release。
