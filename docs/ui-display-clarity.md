# 桌面显示清晰度

分支：`feature/ui-display-clarity`。目标设备为微雪 3.5B 的原生 320×480 竖屏。

## 修改

- FLUX 选中光带停稳时旋转角度为 0、比例为 1，在原生像素网格上显示。邻居光带保留倾斜、缩放和淡出，滚动与回弹连续，命中检测仍读取实际变换。
- SDK 常见 48×48 APP 图标按 48px 显示，不再先放大到 56px。其他尺寸图标一次抗锯齿缩放到 48px。
- 提高亮度/音量卡片提示小字的明暗对比，沿用界面已有的 DIM 配色。
- 12/14/16/18/20px 字库使用 FreeType 的最终尺寸灰度自动微调，24px 以上保留高倍采样。保留抗锯齿，不对字形使用二值化或整体锐化滤镜。
- 字符覆盖、进宽、行高与基线保持一致，不改变公开字体契约，也不需要重新编译 APP。字体缓存和光带缓存数量不变。

## 构建与资源

在已配置 Zephyr/west 的环境运行：

```sh
python tools/build_cpfont.py
python tools/font_metrics.py --check
python tools/resources.py
python tools/firmware.py build --config boards/waveshare_35b/usb-debug.conf --build-dir build/waveshare-openomf
```

WSL 的工程不位于 west 工作区时，设置 `ZEPHYR_BASE=/home/myj/zephyrproject/zephyr`，并启用对应 Python 虚拟环境。正常发布使用设备对应的 selection 配置；本次保留已验证的 USB 调试配置。

将 `dist/sdcard/` 内容复制到 SD 卡根目录。实际改变的文件是 `aster/fonts/ui_14.cpfont`、`ui_16.cpfont`、`ui_18.cpfont`、`ui_20.cpfont`，合计约 15.61MiB；12px 本来已使用相同微调，其他尺寸不变。刷固件不会自动更新 SD 字库，二者需要一起交付。字体按需缓存，不会将这四个完整文件加载进 RAM。

离线更新应关机后复制 SD 卡资源。串口更新使用 SDK 的 ASTERPUT 文件协议：它先写 `.part` 文件，完整接收并校验 CRC 后才发布为正式文件。完成全部资源后重启，重新打开字体文件并清空字形缓存。不要用 shell 的 `fs cp` 原地截断正在使用的字库，或在字体文件仍打开时手动重命名；这会使既有文件句柄失效，甚至产生持续的读取错误。

## 验证

现有 `test_flux.py` 的五项测试检查缓存窗口、稳定层序、变换裁剪与混色、循环索引、边缘返回。`test_font_cache.py` 检查字库栅格化和缓存生命周期；原生微调检查覆盖全部五个小字号。四份完整字库的全部 28,988 个字符进宽、区间索引、行高和基线还与旧文件逐项对照，均相同。板型构建契约检查通过。

实际截图与串口记录保存在忽略目录 `artifacts/ui-clarity/`。`before-home.png` 是修改前的真实设备显示，`font-comparison.png` 从前后 cpfont 的实际字形记录解码，按 1:1 像素对照。移动中的邻居会继续做插值采样，不能与停稳的选中内容具有相同的像素对齐；其他分辨率设备与动态操作体验需要各自实机确认。

2026-10-26 微雪 3.5B 实机验证：正式固件烧录及哈希校验通过，四份新字库均通过 ASTERPUT 完整传输和 CRC 校验。重启后的 `after-home.png` 确认标题、底部中文提示和图标正常，无本次检查中出现的缺字或裁切。`aster sys fonts` 显示十个字号均可用，284 次字形缓存填充，字体 I/O failures=0；本屏缓存占用 177,008B，缓存预算没有增加。串口更新时曾因 shell 原地复制产生文件定位读取失败，已用完整文件协议重新上传并重启恢复；最终截图及统计取自恢复后的正式固件。
