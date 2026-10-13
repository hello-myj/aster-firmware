# P0 APP 故障夹具

仅用于开发测试，ID 限于 `test.p0.*`。需要 ABI 3.7 的独立 Aster SDK；正常固件构建不读取 SDK 源码，也不自动编译或安装这些 APP。

用已安装 SDK 执行：`aster build /absolute/path/to/tests/fixtures/reliability/clean --target xtensa --aot`。使用设备真实串口部署；真实设备需要 AOT，模拟器只能执行同包中的 WASM。

- `clean`：打开只读文件、创建 timer/surface、加载 WAV effect，正常退出应完整清理。
- `initfail` / `trap`：初始化拒绝与事件 Trap；错误后仍应能启动另一 APP。
- `bounds`：权限不足、已关闭/销毁的句柄，以及非法 Guest 指针。非法指针由 WAMR 入参门禁触发 Trap，不应误期望 Host 返回 EINVAL。
- `pure`：不调用 Host 的纯计算事件死循环；关闭超时恢复时，stop 不保证生效。
- `*loop`：init/start/event/pause/resume/stop/destroy 回调死循环。启用恢复策略后通过全芯片复位恢复，不宣称安全单独终止不可信 APP。

主机回归随 `python -m unittest discover -s tests -v` 进入 CI，验证真实 C 安装/恢复、回调期限、错误详情和资源清理代码。板上证据与单串口操作脚本见工作区 `diagnostics/p0-hardware-20261012/`；断电/SD 介质语义仍需手动验收。不要直接运行无限循环夹具于未启用恢复策略的日常设备。
