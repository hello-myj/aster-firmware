# Aster 4.5.0 空白 SD 卡离线准备

先在能联网的电脑或其他设备下载一个 ZIP。之后复制文件、插卡和首次启动都可以离线完成。每张卡只用一包。

| 开发板 | 离线 SD 卡包 | 配套烧录镜像 |
| --- | --- | --- |
| 鱼鹰光电 P4C5，ST7102 480×480，P4 v3.x | `aster-sd-4.5.0-riscv.zip` | `aster-firmware-4.5.0-osptek-p4-st7102.bin` |
| FAKE POD NANO PRO，ESP32-S31，460×460 | `aster-sd-4.5.0-riscv.zip` | `aster-firmware-4.5.0-fakepod-s31.bin` |
| 立创 ESP32-S3 实战派 | `aster-sd-4.5.0-xtensa.zip` | `aster-firmware-4.5.0-lckfb-s3.bin` |

## 复制到卡

1. 准备一张设备可读的 FAT32 空卡，留出至少 100 MiB 可用空间。已在用的卡若能正常开机，可以照常保留；空卡包用于首次初始化，直接覆盖旧卡上的商城目录可能影响已经安装的版本。
2. 在电脑上完整解压对应的 ZIP。把解压得到的整个 `aster` 文件夹复制到 SD 卡根目录，包含以点开头的文件和目录。在 Windows 盘符为 `F:` 时，应看到 `F:\aster\fonts\ui_16.cpfont` 和 `F:\aster\apps\aster.store\.v0\app.aot`。避免多套一层解压目录。
3. 安全弹出 SD 卡，关机插卡再开机。固件识别字体和已安装商城后进入桌面。此时可以离线使用系统；浏览商城目录和下载其他 APP 时再连接 Wi-Fi。
4. 如果仍进首次设置，先核对包与开发板的对应关系、卡格式、目录层级，以及 `aster/apps/aster.store/.active0` 和 `.v0` 是否复制完整。这个离线包针对新卡；中断过在线初始化的旧卡应从原向导继续，已有存档请先保留。

## 包中有什么

- `aster/fonts/` 含 12、14、16、18、20、24、28、32、40、48 px 十份 CPFont 系统字体，两种架构使用相同字体。
- `aster/apps/aster.store/` 是已经安装的 Aster 商城 0.3.3，包含当前架构的 AOT、清单、图标、包文件和有效槽记录。鱼鹰与 FAKE POD 可共用该 SD 包，两块板的固件镜像各自独立。
- `licenses/` 保存字体和商城的来源、许可说明。商城默认连接 Aster 公共 OBS 源。音乐、游戏和个人存档按需另行添加。
- 下载后的 ZIP 或 BIN 可与同一 Release 中 `SHA256SUMS.txt` 对照。Windows 运行 `Get-FileHash 文件名 -Algorithm SHA256`，Linux 运行 `sha256sum 文件名`。

[固件及资源下载](https://github.com/hello-myj/aster-firmware/releases/tag/firmware-v4.5.0) · [商城源码](https://github.com/hello-myj/aster-apps/tree/0722d91a77c45b5cc64d586ccc9bfeeeef8e2847/AppStore)
