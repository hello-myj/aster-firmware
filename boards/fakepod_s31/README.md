# FAKE POD NANO PRO S31 V1

460×460 CO5300 AMOLED、CST820 单点触摸、ESP32-S31、16 MiB Flash 和 16 MiB PSRAM。设备标识 `fakepod-s31`，应用构建目标 `riscv`。此板使用独立的 ESP-IDF 6.1 环境。资料中的 GPIO 对应 V1 接线。

## 构建

在固件仓库根目录加载 ESP-IDF 6.1 环境后执行。生产工程默认构建 Aster 系统，输出目录与鱼鹰 P4C5 分开。

```sh
idf.py --preview -C boards/fakepod_s31/idf -B "$PWD/build/fakepod-s31/system" -DIDF_TARGET=esp32s31 build
idf.py --preview -C boards/fakepod_s31/idf -B "$PWD/build/fakepod-s31/system" -p PORT flash
```

将 `PORT` 替换为下载接口的串口。设备端 Type-C 插头方向决定连接下载串口或原生 USB。USB 读卡器由板级选项 `CONFIG_ASTER_FAKEPOD_USB_STORAGE` 控制。可在 menuconfig 开启，也可在新构建目录配置时传入 `-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.usb-storage"`。进入系统的 USB 读卡器模式后可从电脑写 SD 卡，安全弹出后再恢复设备访问。

## 实机功能

- 首次联网准备 SD 字体与商城；本地音乐放在卡根目录的 `music/`，可通过启用了 USB 读卡器的固件写入。设备运行 RISC-V AOT 应用，460×460 播放器通过商城或 SDK 串口安装。
- CS43131 有线输出支持 44.1、48、88.2 和 96 kHz 的立体声验证。系统播放器可播放本地 MP3、WAV 和 FLAC，具体位深和采样率取决于解码器与输出。
- 系统蓝牙支持手机向设备播放，以及设备向蓝牙音箱输出 SBC。播放器使用系统选定的输出。QMI8658 可用于摇动切换动态歌词。
- 上方实体键可短按熄屏或唤醒，持续按住约 3 秒关机；关机后用同一按键开机。熄屏期间可继续播放。

接线和适配器选择在 [板级注册](idf/aster_board.cmake)。诊断、已验证组合及仍待测项见 [Hi-Fi 设计与实施记录](../../docs/fakepod-s31-hifi-design.zh-CN.md)。
