# 从工程到设备

当前 ABI 为 4.0.0。APP 使用 CPU 目标 `xtensa`，可安装到立创和微雪两款 ESP32-S3。屏幕方向写在应用 manifest 里，运行时再查询实际尺寸。

准备 Python 3.12、SDK 附带的工具链，以及 Node 22。三个仓库放在同一父目录时：

```bash
python aster-sdk/tools/aster.py init my-game --template swipe-game --id test.quickstart --target xtensa
python aster-sdk/tools/aster.py build my-game --aot
python aster-sdk/tools/aster.py verify my-game/dist/xtensa/test.quickstart.ast --target xtensa
node aster-sim/src/cli.js my-game/dist/xtensa/test.quickstart.ast --headless --frames 5 --screenshot quickstart.png
```

改完 `my-game/src/main.c` 后重新 build 和 verify。模拟器执行包里的 WASM，忽略 Xtensa AOT。

设备已刷入匹配固件并装好系统字体后：

```bash
python aster-sdk/tools/aster.py deploy my-game/dist/xtensa/test.quickstart.ast --target xtensa --port <串口> --run
```

Windows 便携 SDK 用 `aster.exe`，串口换成系统枚举到的端口。微雪当前配置不提供加速度计和陀螺仪。

<!-- quickstart-check:start -->
```json
[
  ["{python}", "{sdk}/tools/aster.py", "init", "{project}", "--template", "swipe-game", "--id", "test.quickstart", "--target", "xtensa"],
  ["{python}", "{sdk}/tools/aster.py", "build", "{project}", "--aot"],
  ["{python}", "{sdk}/tools/aster.py", "verify", "{project}/dist/xtensa/test.quickstart.ast", "--target", "xtensa"],
  ["{node}", "{sim}/src/cli.js", "{project}/dist/xtensa/test.quickstart.ast", "--headless", "--frames", "5", "--screenshot", "{output}/quickstart.png"]
]
```
<!-- quickstart-check:end -->
