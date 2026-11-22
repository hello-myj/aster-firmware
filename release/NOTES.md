# Aster 固件 4.5.0

从 4.3 升级，本版新增 FAKE POD NANO PRO 的正式固件镜像。系统按开发板能力启用电源、蓝牙、USB 读卡器与媒体服务。FAKE POD 支持本地 WAV/FLAC/MP3、经典蓝牙收发及播放器查询；PNG 封面解码和应用恢复显示也已修复。应用启动时检查 AOT 架构、必需能力和可选板型名单。

**选择镜像。** `osptek-p4-st7102` 用于鱼鹰光电 P4C5 的 P4 v3.x、480×480 ST7102；`lckfb-s3` 用于立创 ESP32-S3 实战派；`fakepod-s31` 用于 FAKE POD NANO PRO。每个 BIN 均按该板的启动布局生成，烧录起始地址为 `0x0`。

**首次使用。** 装好 SDK 4.5 后运行 `aster firmware list`，再执行 `aster firmware flash --board fakepod-s31 --port COM11`。把板号和串口换成自己的，Linux 可用 `/dev/ttyACM0`。烧录会改写设备 Flash。

**可以离线开机。** 鱼鹰和 FAKE POD 下载 `aster-sd-4.5.0-riscv.zip`，立创下载 `aster-sd-4.5.0-xtensa.zip`。将解压出的 `aster` 文件夹复制到 FAT32 空卡根目录，安全弹出，关机插卡再启动。卡内已准备十份系统字体和对应架构的商城 0.3.3。也可以插空卡，按屏幕提示联网下载。附件 `SD-OFFLINE.md` 有完整目录示例；`SHA256SUMS.txt` 可用于核验下载。

[离线 SD 卡说明](https://github.com/hello-myj/aster-firmware/blob/firmware-v4.5.0/docs/offline-sd.zh-CN.md) · [配套 SDK](https://github.com/hello-myj/aster-sdk/releases/tag/sdk-v4.5.0)
