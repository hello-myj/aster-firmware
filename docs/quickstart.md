# 从独立工程到设备 APP

适用范围：ABI 3.7.0；APP CPU 目标为 `xtensa`，对应两款 ESP32-S3 开发板。开发板、屏幕方向、主机系统是独立选择：SDK 的 `--target` 不接收开发板名称；APP 在 manifest 声明方向，运行时查询屏幕和可用能力。

准备 Python 3.12、SDK 固定版本附带的 WASI/AOT 工具链，以及 Node 22 和匹配版本的模拟器。在 SDK 执行 `python tools/aster.py install` 安装 Python 依赖，模拟器执行 `npm ci`。组合版本记录在固件 `contract/release-matrix.json`；实际版本、工具链哈希和构建输出记录在发布检查报告。

下面假设三个仓库并列放置；在它们的父目录执行，工程创建在 SDK 之外：

```bash
python aster-sdk/tools/aster.py init my-game --template swipe-game --id test.quickstart --target xtensa
python aster-sdk/tools/aster.py build my-game --aot
python aster-sdk/tools/aster.py verify my-game/dist/xtensa/test.quickstart.ast --target xtensa
node aster-sim/src/cli.js my-game/dist/xtensa/test.quickstart.ast --headless --frames 5 --screenshot quickstart.png
```

需要修改行为时编辑 `my-game/src/main.c`，然后重复 build 和 verify。默认模板已包含图形、触摸和音效的调用示例。Linux、Windows 的 APP 源码和 `xtensa` 包相同；Windows 便携 SDK 使用 `aster.exe` 代替 `python .../tools/aster.py`，串口使用实际 `COM` 编号。模拟器按 WASM 执行并忽略 Xtensa AOT；模拟器通过不证明真机 AOT 能运行。

设备预先刷入匹配固件并准备系统字体资源。关闭占用串口的监视器，确认真实串口后部署：

```bash
python aster-sdk/tools/aster.py deploy my-game/dist/xtensa/test.quickstart.ast --target xtensa --port /dev/ttyACM0 --run
```

微雪 USB Serial/JTAG 通常为 `/dev/ttyACM0`，LCKFB USB 串口通常为 `/dev/ttyUSB0`；实际以系统识别为准，不能把端口名称作为板型检测。部署会验证产品、CPU、ABI、SD 和必需能力，失败不会继续运行。字体由固件发布的资源准备，不能从模拟器截图推断设备已具备同一字库。

微雪当前配置禁用 IMU；模拟器有虚拟 IMU，不能据此在微雪 manifest 宣称 accelerometer/gyroscope 可用。虚拟摄像头、音频输入和 Wi-Fi 行为用于主机测试，不能证明相应真机外设实现。权限仍需在 APP 声明。

## 自动检查使用的相同命令

下面的数组就是上面创建、构建、校验、模拟命令的参数形式；发布脚本替换绝对路径并执行，不经 shell。`project` 指向新临时工程，`output` 指向报告目录。

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

CI 自动执行到模拟。真实部署需要连接开发板，不能在 CI 假装通过；对应验收证据与未测范围见 [可靠性记录](reliability-validation.md)。发布前在可用设备执行上面的 deploy 命令，保存实际镜像/包哈希、串口结果和用户画面反馈。
