# 从工程到设备

当前 ABI 为 4.6.0。立创 ESP32-S3 实战派与微雪 S3 使用 `xtensa`；鱼鹰光电 P4C5 和 FAKE POD NANO PRO 使用 `riscv`。下例为立创板构建一个 AOT 包，同时用其中的 WASM 在主机做首帧检查。跨架构部署时应为目标架构重新构建。

准备 Python 3.12、SDK 附带的工具链，以及 Node 22。三个仓库放在同一父目录时：

```bash
python aster-sdk/tools/aster.py init my-game --template swipe-game --id test.quickstart --target xtensa
python aster-sdk/tools/aster.py build my-game --aot
python aster-sdk/tools/aster.py verify my-game/dist/xtensa/test.quickstart.ast --target xtensa
node aster-sim/src/cli.js my-game/dist/xtensa/test.quickstart.ast --headless --frames 5 --screenshot quickstart.png
```

改完 `my-game/src/main.c` 后重新 build 和 verify。模拟器执行包里的 WASM，忽略 Xtensa AOT。

设备刷入对应开发板的固件后，插入 SD 卡完成[首次开机](first-boot-setup.md)。在 SDK 中通过串口安装刚才验证的包：

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
