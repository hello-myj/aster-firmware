# LCKFB 实机验收记录（2026-10-13）

已使用明确的 LCKFB Kconfig 和设备树完成自动补测；最终人工画面、随动和触摸确认通过。有效诊断镜像与配置见 diagnostic-image.json、diagnostic.config.txt；记录必须以此次纯配置重测结果为准。

superseded-mixed-config/ 中是已作废的首轮数据：共用 build/ 根目录残留的微雪 p2-graphics.conf 被 Zephyr 自动追加合并。虽然物理设备、显示节点和大部分能力来自 LCKFB，但硬件选择混合，不能作为纯 LCKFB 完整验收。此前两处断电和图形性能数据仅保留为排查证据。

已添加构建目录碎片检查及编译板型核对，阻止同类问题再次静默通过。正式配置已恢复，故障注入和图形性能日志关闭；尚未通过的项不使用首轮数据替代。

## 纯 LCKFB 补测进度

- 已通过：40 个安装错误点、4 个整机复位点、正常/拒绝/Trap 各 3 次循环、7 个回调阶段超时恢复。见 reliability.json 和 reliability.log。
- 已通过：manifest 关闭后的真实断电；恢复完整旧版本 1.0.0，AOT、存档、缓存、资源与升级重试正常。见 power-validation/manifest-result.json。
- 已通过：commit 关闭后的真实断电；恢复完整新版本 2.0.0，AOT、存档、缓存、资源与再次安装正常。见 power-validation/commit-result.json。
- 已通过：P1 冻结旧 hello/2048 AOT 与快速入门工程部署；见 compatibility/hardware.json。
- 已通过：P2 三轮图形对比，全部六张截图像素一致；见 graphics/graphics.json。
- 已通过：恢复正式配置，event 5000 ms 超时恢复及 7 个日常 AOT APP 启停；原有 17 个 APP ID 保留，临时测试 APP 已清理。见 production.json 和 test-cleanup.json。
- 已通过：正式固件 IMU 前台运行期间连续 60 次六轴读取，未出现读取超时或 APP 崩溃；见 imu-samples.json/log。
- 已通过：用户确认正式固件 IMU 画面、随动和“归零”触摸均正常；已退出 IMU 返回主页，见 production.json 和 final-home.log。

正式镜像哈希和配置见 production.json、production.config.txt；使用分支基础提交加本地未提交的构建板型保护，不能仅用基础 HEAD 表示完整源码。60 次 IMU 采样是通信稳定性短测，不代表长期资源稳定性或全部外设验收；真实 SD 满盘、自然介质故障、更多断电点及长期运行仍见 reliability-validation.md。
