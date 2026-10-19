# N6 微雪验收（2026-10-19）

ABI 4.0.0。只测微雪 `/dev/ttyACM0`，板型 `aster.esp32s3.waveshare35b`。立创未接、未测。记录里不写网络名称。

## 版本

| 组件 | 版本 | 说明 |
| --- | --- | --- |
| 固件 ABI | 4.0.0 | `ASTER_ABI_MAJOR 4`，六个 `aster_net_*` 导入 |
| SDK | 4.0.0 | `aster_netlib` 包装旧函数名，23 个示例已重编 |
| 模拟器 | 4.0.0 | `src/net.js`，导入表只有六个新名字 |

旧 3.13 快照在 `contract/abi-3.13.0.json`，只用于对照。`contract/abi-baseline.json` 已换成 4.0.0。对照工具仍把这次主版本升级判为破坏性变更，这是预期结果。

## 主机和模拟器

- 固件 `python3 -m unittest discover -s tests`：126 项，2 项跳过，0 失败。
- SDK `python3 -m unittest discover -s tests`：105 项通过，3 项跳过。手册检查打印 ABI 4.0.0。
- Linux Node：`node --test test/*.test.js`，100 通过，0 失败，0 跳过。
- Windows Node 24：同一套测试，100 通过，0 失败，0 跳过。

## 镜像

没有覆盖 `build/qspi-prefetch40`。该目录里的 `zephyr.bin` 仍是 `6291edaffaca107e8baae63ee6b185ac56211363a6695ddbb350cd8401d94057`。

- 诊断镜像 `build/network-n6-wave`，SHA256 `7bbdac32b20f77d7f00fdad99ef4d2ab8bbefa05c4692d0a7fc12f355dff5dbe`。在原有 HTTP、连接池、mDNS 片段上加了 WebSocket，否则 WS 示例无法安装。
- 日常镜像 `build/network-n6-daily`，SHA256 `4468afbdaf1fd20fc602bddf6ccb7a1e48947ad380f6f0a3136f70b17c820f90`。配置与 `qspi-prefetch40` 相同，只是 ABI 4.0.0。

`dram0_0_seg` 仍是 `0x3fc88000`，长度 `0x61704`。诊断链接占用 195752 字节（49.05%）。刷入后 DRAM 堆峰值仍是 41764。PSRAM 静态从刷机前的 274496 变为 274880。网络服务栈计数为 4096。libc 堆峰值在跑过示例后为 72404，刷机前空闲时是 54940。没有新增线程、TLS 池或协议栈。

## 拒绝和示例

冻结的非网络包 `aster.hello`（ABI 3.2）和网络包 `aster.http.fetch`（ABI 3.9）安装失败，已安装的 3.x `aster.hello` 启动失败。日志和 shell 都是「需用新 SDK 重编」。

100 次 UDP 打开后立即关闭全部成功。第 3 个 UDP 返回 -28。对 192.0.2.1:9 的 TCP 在取消后立刻是 `state=cancelled`。随后的超时连接进入失败计数。结束后 `active=0`，打开数和关闭数相等。

七个网络示例都已安装并用 AOT 启动：Network Echo 打出公开句柄和本机地址；HTTP Fetch 收到连接被拒绝 -24；HTTP Download、HTTP Resume、MQTT Echo、WS Echo、在线音乐都能启动再停止。WS Echo 发出 `kind=5` 的状态事件。权限测试 19 项通过、0 失败。Trap 测试启动后回到启动器，连接计数回到 0。音量 53 和原来的地址在两次刷写后都还在。

启动器同时只跑一个前台 APP，所以在线音乐和 WSS 没有同时留在屏幕上。触摸和上下拉没有人在场确认。

## 没有换上的 APP

- `com.example.breakout` 1.0.3 没有源码。它还在设备上，启动会被 ABI 拒绝。
- `aster.imucube` 已用 SDK 重编，但日常固件和原来的 `qspi-prefetch40` 一样没有声明加速度计、陀螺仪，安装被拒绝。设备上仍是旧包。
