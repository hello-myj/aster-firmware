# N6 网络 ABI 4.0

ABI 从 3.13.0 升到 4.0.0。主版本必须相等，所以任何 3.x 包在安装和启动时都会被拒绝，包括不用网络的。Wi-Fi 配置导入 `aster_wifi_*` 不在这次改动里。

整合前诊断镜像 `build/network-n52-wave` 的 `dram0_0_seg` 为 `0x3fc88000`，大小 `0x61704`。这是 3.13 诊断固件的静态 DRAM，用作对比基线。

刷 4.0 之前，微雪日常镜像（ABI 3.11.0，`apps=14`，`sd_mounted=yes`）实测：

- `aster sys mem`：DRAM heap 98304，峰值 41764，已用 15276；libc heap 可用 109316，峰值 54940，已用 51840；PSRAM 静态 274496，堆 7864320，已用 8138816 / 8388608。
- `aster net stats`：活动连接 0，打开/关闭/失败/取消/丢弃均为 0。
- 已装 14 个 APP 都能从 SDK 源码重编，只有 `com.example.breakout` 1.0.3 没有源码。模拟器夹具只改了它的清单版本号，没有重新编译，设备上不能当成 4.0 包重装。其余 13 个是 `aster.apitest`、`aster.chess`、`aster.gallery`、`aster.game.2048`、`aster.gfxshow`、`aster.hello`、`aster.imucube`、`aster.permdeny`、`aster.player`、`aster.reader`、`aster.snake`、`aster.trap`、`aster.music.online`。

## 六个导入

| 导入 | 签名 |
| --- | --- |
| `aster_net_info` | `(ii)i` |
| `aster_net_open` | `(ii)i` |
| `aster_net_io` | `(iiii)i` |
| `aster_net_status` | `(iii)i` |
| `aster_net_cancel` | `(i)i` |
| `aster_net_close` | `(i)i` |

协议号与 `ASTER_SYS_NET_STATE` 的种类相同：TCP 1、UDP 2、HTTP 3、MQTT 4、WS 5。操作码：SEND 1、RECV 2、HEADER 3、SUBSCRIBE 4、UNSUBSCRIBE 5、OPERATION 6。协议不支持的操作返回 `ASTER_ENOTSUP`。

权限仍是 `net`。能力位不变：network 15、http 16、tls 17、mqtt 19、websocket 20。传输槽仍是全局 2 个，TLS 槽 1 个。每个 APP 最多 4 个公开句柄，句柄带代次。过期、其他 APP 或 CLI 句柄返回 `ASTER_EBADF`。状态事件的 `u.sys.value` 是公开句柄。cancel 被接受后，status 立刻显示终态。

## 旧导入映射

| 旧导入 | 新入口 |
| --- | --- |
| `aster_network_get_info` | `aster_net_info` |
| `aster_network_open` | `aster_net_open`，proto 为 TCP 或 UDP |
| `aster_network_send` | `aster_net_io` SEND |
| `aster_network_recv` | `aster_net_io` RECV |
| `aster_network_status` | `aster_net_status` |
| `aster_network_cancel` | `aster_net_cancel` |
| `aster_network_close` | `aster_net_close` |
| `aster_http_request` | `aster_net_open`，proto HTTP，尾部带方法和正文 |
| `aster_http_status` | `aster_net_status` |
| `aster_http_read` | `aster_net_io` RECV |
| `aster_http_header` | `aster_net_io` HEADER |
| `aster_http_cancel` / `aster_http_close` | `aster_net_cancel` / `aster_net_close` |
| `aster_mqtt_connect` | `aster_net_open`，proto MQTT |
| `aster_mqtt_publish` | `aster_net_io` SEND |
| `aster_mqtt_subscribe` / `unsubscribe` | SUBSCRIBE / UNSUBSCRIBE |
| `aster_mqtt_operation` | OPERATION |
| `aster_mqtt_recv` | RECV |
| `aster_mqtt_status` / `cancel` / `close` | status / cancel / close |
| `aster_ws_connect` | `aster_net_open`，proto WS |
| `aster_ws_send` / `aster_ws_recv` | SEND / RECV |
| `aster_ws_status` / `cancel` / `close` | status / cancel / close |

APP 源码继续调用原来的函数名。这些函数由 SDK 的 `lib/aster/aster_netlib.c` 实现，内部只导入上面六个符号。

结构布局在 `contract/include/aster/net.h`。打开请求是 40 字节头，HTTP 再加 24 字节，MQTT 再加 48 字节。TCP、UDP、WS 没有尾部。`size` 必须正好等于头加尾。
