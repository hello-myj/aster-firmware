# APP 回调期限与故障恢复

这是一项固件执行策略，不改变公开 APP ABI 3.7.0 或 ASTP/AOT 包格式。当前普通 Xtensa AOT 生成物没有为非共享内存的纯计算循环提供可靠的异步终止检查；不能把设置退出标志或 `wasm_runtime_terminate()` 当作强制终止保证。

## 默认期限

- init/start：15000 ms（`CONFIG_ASTER_APP_INIT_TIMEOUT_MS`）。
- event/pause/resume：5000 ms（`CONFIG_ASTER_APP_EVENT_TIMEOUT_MS`）。
- stop/destroy：3000 ms（`CONFIG_ASTER_APP_STOP_TIMEOUT_MS`）。

期限从调用 Guest 函数开始，包含其同步 Host 调用耗时；模块加载和初始化前的字体预热不属于这次回调计时。长工作应拆到多帧/定时事件，避免在单次回调中长期录音、等待网络或处理大文件。可按产品需要调整期限，不应为了隐藏失控程序而无限放大。

`CONFIG_ASTER_APP_WATCHDOG` 默认启用。独立内部 RAM 线程以高于 APP worker 的优先级每 10 ms 检查期限，状态只包含复制的 APP ID、阶段和时间，不持有需要从外部线程释放的 WAMR/Guest 指针。

回调正常返回或 Trap 时解除计时，沿原 worker 清理资源；已经返回但超时的回调也按失败处理。回调始终不返回时，ESP32-S3 通过完整芯片复位恢复。启动读取带完整性校验的 RTC 保留原因，打印 `ASTER_RECOVERED` 并将该 APP 标记为 CRASHED、err=8，随后回到原生主页；不自动重跑该失控 APP。

完整芯片复位会重启其他设备服务，无法保证未关闭/未同步的文件写入保留。已正常关闭的存档在实际测试中保持完整。期限加检查周期不是整个设备恢复耗时，启动、SD 初始化和字体预热还需要时间。

该线程策略适用于仍能被调度抢占的 Guest 执行；不能保证中断被关闭、内核/驱动损坏或整个硬件失锁时的恢复。因此不宣称强制终止不可信 APP 的安全隔离保证；需要这一保证的产品必须另行验证 AOT 生成器、安全边界及独立硬件 watchdog。

## 清理和诊断

- Trap 详情在清除 WAMR exception 缓冲之前复制，保留原始错误。
- init 拒绝、生命周期 Trap 与事件 Trap 记录 CRASHED/错误码。
- worker 顺序释放打开文件、精灵缓存、执行环境、实例、模块、镜像；正常/异常返回释放字体会话与音频 effect 缓冲。
- 停止/销毁不返回时不从另一个线程强行释放仍在执行的模块；通过系统复位恢复冷启动资源状态。
- Shell 栈改为 8192 B 并启用 sentinel；安装/版本 CRC 的实机诊断路径曾记录约 6.3 KiB 高水位，原 6144 B 配置缺少余量。

安装故障注入只在 `CONFIG_ASTER_PKG_FAULT_INJECTION=y` 的诊断固件存在，且只能匹配指定 `test.p0.*` APP。日常固件默认关闭该项。输入 `aster app fault` 的临时接口不属于 APP SDK。

## 回归

`tests/test_callback_watchdog.py` 验证真实计时与保留记录代码的回绕、返回后超时、复位原因以及损坏记录拒绝。`tests/test_runtime_faults.py` 验证回调解除计时、Trap 详情和重复清理。安装/升级回归见 `tests/test_install_io.py`；这些全部随常规 unittest 进入 CI。

`tests/fixtures/reliability/` 保存可用独立 SDK 构建的真机 AOT 夹具，包括 clean、initfail、trap、bounds、pure 和七种生命周期死循环。主机测试不代替 AOT/DMA/外设实机检查。

微雪实际验证结果的精简证据在仓库 `docs/validation/p0-2026-10-13/`，长期清单见下方链接：40 个存储注入点、4 个完整芯片复位恢复点、正常/启动失败/Trap 多次循环、七个回调阶段死循环，以及权限、旧句柄和非法指针检查。另经用户手动拔掉 USB 供电至少 5 秒，验证 manifest 关闭后恢复完整旧版本、commit 关闭后恢复完整新版本；两次 AOT 启动、存档、缓存、资源与安装重试均通过。此结果覆盖这两个暂停点，不证明所有 FAT 物理写入时刻或所有 SD 卡均具备相同断电语义；软件芯片复位不替代断电。

硬件验收状态、剩余待测项与仓库内证据统一维护在 [P0 可靠性验收记录](reliability-validation.md)。
