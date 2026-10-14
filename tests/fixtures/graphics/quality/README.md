# 字体与图片质量固定页

G8/G9共同使用的独立AOT测试APP，保留 `test.gfx.g8quality` ID和原测量页面，便于冷/热对照。两个32×32 BMP为项目自制的像素图案与渐变测试输入，不是设备截图或外部图片。

从工作区父目录执行：

```bash
aster-sdk/.venv-linux/bin/python aster-sdk/tools/build.py aster-firmware/tests/fixtures/graphics/quality --target xtensa --aot
```

安装到空闲设备，启动后等待 `G8QUALITY DONE coldms=... warmtotal=... samples=12 errors=0`。第一帧计冷页，后12帧计热页；时间包含APP命令编码、绘制、提交，排除init中的图片解码，不代表面板光学FPS。每轮先停止、再启动APP释放会话字形缓存，至少重复三轮；设备重启、文件位置、字库哈希和SD状态分别记录，不能把重开APP称为掉电冷启动。

页面覆盖原生12/14/16px和18px标题、相同前后景、显式背景、原尺寸与半透明图片。实际字库由固件SD资源提供。读 `aster sys fonts` 时记录前后累计差值并处理32位回绕，计数包含launcher切换。测试后卸载本临时APP和SD源包，恢复原前台与音量；禁止覆盖用户原有同ID应用。
