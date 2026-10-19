# 微雪 ESP32-S3-Touch-LCD-3.5B

Zephyr board：`waveshare_esp32s3_touch_lcd_35b/esp32s3/procpu`。硬件型号 `waveshare-35b`，SDK 目标 `xtensa`，当前 ABI 为 4.0.0。

显示初始化来自[官方示例](https://files.waveshare.net/wiki/ESP32-S3-Touch-LCD-3.5B/ESP32-S3-Touch-LCD-3.5B-Demo.zip) 的 `ESP-IDF/01_factory/components/esp_bsp`。共享 Zephyr 还需要 [patches/README.md](../../patches/README.md) 里的补丁。默认关闭 QMI8658，不声明加速度计和陀螺仪。

## 构建和烧录

```bash
python tools/firmware.py build --config boards/waveshare_35b/selection.conf --build-dir build/waveshare-35b
python tools/firmware.py flash --config boards/waveshare_35b/selection.conf --build-dir build/waveshare-35b
```

镜像是 `build/waveshare-35b/zephyr/zephyr.bin`。也可以在 `python tools/firmware.py menuconfig` 里选择这块板。

可选的 40 MHz 预拷贝配置：

```bash
python tools/firmware.py build --config boards/waveshare_35b/qspi-prefetch.conf --build-dir build/waveshare-35b-prefetch
python tools/firmware.py flash --config boards/waveshare_35b/qspi-prefetch.conf --build-dir build/waveshare-35b-prefetch
```

只接板载 USB、需要 shell 时：

```bash
python tools/firmware.py build --config boards/waveshare_35b/usb-debug.conf --build-dir build/waveshare-35b-usb
python tools/firmware.py flash --config boards/waveshare_35b/usb-debug.conf --build-dir build/waveshare-35b-usb
```

应用包：

```bash
aster build <应用目录> --target xtensa --aot
aster deploy dist/xtensa/<应用>.ast --target xtensa --port <串口> --run
```

屏幕尺寸用 `aster_display_get_info()` 读取。原生分辨率为 320×480。

## 引脚

- 屏幕 AXS15231B，SPI2 QSPI 40 MHz：CLK=5，D0..D3=1/2/3/4，CS=12。像素为 RGB565，每像素 2 字节。
- 背光 GPIO6，LEDC 5 kHz，高有效。
- I2C0：SDA=8，SCL=7，400 kHz。触摸 0x3b，PCF85063 0x51，AXP2101 0x34，ES8311 0x18。
- SDMMC 1 位：CLK=11，CMD=10，D0=9，挂载 `/SD:`。
- 音频 ES8311：MCLK=44，BCLK=13，WS=15，SoC TX=16，SoC RX=14。PCM 为 16 kHz、16 bit、立体声。
- BOOT 为 GPIO0。外接 shell 使用板 TX=GPIO43、板 RX=GPIO17，115200。GPIO44 是音频 MCLK，不能当作 UART RX。

板载 Type-C 用于烧录和文件传输。`usb-debug.conf` 把 console 和 shell 也放到这块 USB 上，同一端口完成日志、命令和安装。

## 显示

竖屏为 320×480。横屏应用由软件旋转为 480×320，退出后恢复竖屏。桌面与立创板使用同一套流光界面，按本板分辨率排版。

系统字体放到 FAT SD 卡，步骤见仓库 README。
