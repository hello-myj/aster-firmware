# N2 APP/SDK 验证（已完成，2026-10-15—16）

两板CLI G1及图形交互已通过后，才接入公共ABI3.9、SDK、模拟器及示例。两板APP层、最终恢复及实体交互均已通过，N2本阶段完成；已本地提交，未推送或发布Release。此前CLI镜像ABI3.8证据见[CLI记录](network-n2-cli-2026-10-15.md)。

## 公共契约与实现

新增六个导入：`aster_http_request/status/read/header/cancel/close`。方法、状态、结构、能力位及错误均使用固定宽度，`aster_http_status_t`为56B，两个u64分别位于40/48。新增http能力位16、tls位17；只使用net权限，下载另需fs。既有ABI值和导入签名不变。

错误新增EPROTO=-26、ETLS=-27、ECERT=-28、ECLOCK=-29、EREDIRECT=-30。平台errno和Mbed TLS位不直接暴露；证书结果转换为稳定标志。未执行验证时的内部UINT32_MAX不当成全部证书错误。

固件公共头文件经`tools/export_contract.py`导出ZIP，再由SDK的`tools/import_contract.py`校验并导入，包括API/lock和CPU平台元数据。SDK不读取Zephyr/ESP-IDF头文件。默认网络配置启用HTTP/HTTPS，96KiB加密池置于PSRAM；包验签共享该池及互斥锁。诊断采样/内存调试仍独立关闭。

所有请求输入同步复制，后台只持数字owner和服务私有数据。APP退出接受owner-close后即可退役会话，不等待DNS/TLS推进；后台回收未完成时仍占配额。新会话编号不复用，旧回调不能访问新APP内存。无权限、越界输出或重叠header输出不会消费响应数据。

## LCKFB 实机

APP诊断镜像SHA256：`c8d921b5ff6add116cc0bbd9342d7da56d9dce46fa4cd319467de38448bd8660`。

- SDK独立编译Xtensa AOT `http-fetch`，通过HTTPS获取JSON，校验状态200、Content-Type、23字节固定内容，日志`HTTPAPP PASS mode=json bytes=23`。
- 同样编译`http-download`，通过HTTPS以512B APP缓冲下载65536字节，逐字节校验`i % 251`，CRC32=`7faa50d3`；关闭文件成功。shell随后确认沙箱文件`download.bin`长度65536。
- 响应大于2KiB服务队列，没有整份响应分配。两示例完成后HTTP active/closing/tls/私有PSRAM均为0，CA配置567B另计，后台8KiB栈保留。
- `tests/fixtures/http_lifecycle_probe.c`编译为五个AOT变体：坏指针及短结构返回EINVAL而不trap、缺net权限返回EPERM、TLS握手阶段主动trap、响应体已有缓冲时主动退出、创建请求后初始化失败。每项均先检查对应标志，再等待APP离开RUNNING并确认请求缓冲归零，不能只凭启动成功判定。
- 主动trap与初始化失败分别按预期进入CRASHED（运行时错误14/12），不计作正常应用运行。测试后原生opened=7/closed=7，active/closing/私有PSRAM=0。后两项仍有挂起请求时不靠APP stop手动close，由运行时回收。
- 本轮栈高水位APP7828/16384 B、HTTP2836/8192 B、网络1316/4096 B；CLI矩阵另测HTTP3364/8192 B，不能用较小数字覆盖历史峰值。shell因安装/异常清理使用6228/8192 B，应保留现有8KiB配置。
- 原17个APP、已保存Wi-Fi及音量74保留；临时7个APP、根目录包及CA公钥文件已清理，信任根恢复默认。
- 恢复ABI3.9日常镜像`14715c91c0b542177fffce12bf6d6d54cd1fd9f34305213712282760af142511`，关闭协议池/内存调试片段，HTTP/HTTPS功能保留。旧2048 AOT启动并确认RUNNING/首帧，保持前台。串口已释放。

## 微雪 3.5B 实机

APP诊断镜像SHA256：`0dfa16df7b6ca143b973c4dfb8b827c5ecbdfc1af65c40be116fbdf568604868`。

- 使用与LCKFB完全相同的SDK Xtensa AOT包：HTTPS JSON内容23字节、Content-Type校验通过；HTTPS流式下载65536字节、CRC32=`7faa50d3`。shell确认落盘文件长度65536。
- 五类生命周期变体全部通过：坏指针/短结构、缺net权限、TLS握手阶段trap、响应体已有缓存时退出、创建请求后初始化失败。各项结束HTTP私有缓冲均归零，最终native opened=6/closed=6、active/closing/私有PSRAM=0。
- APP栈高水位7828/16384 B，HTTP2836/8192 B，网络1316/4096 B；CLI阶段HTTP曾达3364 B，应取跨场景较大值。shell高水位6276/8192 B，保留8KiB栈。
- 临时7个APP及根目录包、CA公钥文件已清理，CA恢复默认；原13个APP、音量22及已保存Wi-Fi保留。
- 恢复ABI3.9日常镜像`4b2987d5f6e7b1501ba1f3f962908b8fff20826150d76e63b9b2317500ece6c0`，关闭协议池/内存诊断片段，HTTP/HTTPS功能保留。原Gfx Show AOT确认RUNNING，用户最终确认画面、触摸和上下拉恢复正常，保持前台；串口释放。

## 主机与模拟器

- 固件104项主机测试通过，包括ASan/UBSan运行实际HTTP服务、原生桥接和TCP服务，覆盖复制输入、越界、外国会话句柄、重叠输出、稳定错误转换及非阻塞owner回收。
- SDK104项测试通过，文档/契约检查匹配ABI3.9、CPU xtensa。两个示例均生成WASM及Xtensa AOT。
- Linux模拟器78项全部通过、无跳过；强制提供旧2048、IMU、Breakout、Scene及两个HTTP真实APP包，包含既有应用兼容回归。
- Windows原生Node执行HTTP测试文件，8项全部通过、无跳过，包括真实TLS证书验证及两个实际WASM APP。WSL共享路径下`node --test UNC路径`未找到文件，改为直接执行测试文件，由node:test运行相同用例；不将该主机路径问题归因于APP协议失败。
- HTTP与TCP/UDP共用进程级两槽和单TLS配额，真实IPv4/TLS显式开启`--network`，不与确定性replay共用。默认根为ISRG Root X1，开发CA通过`--network-ca`设置，不支持跳过验证。
- Node系统socket/TLS缓冲不等于设备PSRAM预算，其TLS记录与底层错误细节也不承诺逐一等同Mbed TLS。公开队列上限2048B、状态、错误域、重复头、重定向和APP生命周期由回归约束；设备峰值以实机为准。

## 完成边界及后续

2026-10-16用户确认微雪最终日常固件的画面、触摸、上拉取消和下拉收回正常。Windows/Linux临时测试服务器均已停止；删除临时防火墙规则首次因Windows错误5被拒，用户随后在管理员PowerShell执行删除并确认完成。

N4的100轮回收、10分钟复合压力独立排期，不能用本轮功能测试代替。默认根集合、TLS1.2、单TLS配额及公网letsencrypt.org握手EPIPE限制继续保留，不宣称所有站点兼容。本地提交时已将模拟器sdkCommit固定到ABI3.9 SDK提交`0eeda6f1268657b77ce0591064c3fd30968fafa9`，sdkWorkingTree=false。后续远端配套发布前须先推送该SDK提交；不能以未推送提交触发跨仓库固定版本CI。本轮未运行远端CI。

SDK示例普通失败/退出会删除不完整下载文件；突然断电不保证事务性，未新增rename/fsync/OTA能力。原始日志、证书/私钥、ZIP、镜像及临时fixture工程均位于仓库外`diagnostics/network-n2-20261015/`，不纳入Git。
