# APP 回调期限

设备上的普通 Xtensa AOT 不能在纯计算循环里被异步打断。回调超过期限时，ESP32-S3 整片复位，记下阶段和应用，回到桌面，不自动重开该应用。

## 默认期限

- init/start：15000 ms（`CONFIG_ASTER_APP_INIT_TIMEOUT_MS`）
- event/pause/resume：5000 ms（`CONFIG_ASTER_APP_EVENT_TIMEOUT_MS`）
- stop/destroy：3000 ms（`CONFIG_ASTER_APP_STOP_TIMEOUT_MS`）

计时包含这次回调里的同步宿主调用。长时间的网络、文件或录音放到后续帧或定时事件里做。`CONFIG_ASTER_APP_WATCHDOG` 默认打开，每 10 ms 检查一次。

回调正常返回或 Trap 时解除计时，并在原来的工作线程里释放文件、精灵、执行环境和字体会话。已经返回但超时的回调按失败处理。

复位会重启其他设备服务。已经正常关闭的存档会保留。

## 诊断

- Trap 文本在清除运行时异常缓冲之前复制。
- init 拒绝、生命周期 Trap 和事件 Trap 记为 CRASHED。
- stop/destroy 不返回时，不从另一条线程拆掉仍在执行的模块，靠复位回到干净状态。

`aster app fault` 只存在于打开了 `CONFIG_ASTER_PKG_FAULT_INJECTION` 的诊断固件，不属于应用 SDK。
