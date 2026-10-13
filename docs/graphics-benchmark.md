# 固定图形场景基准与合批结果

2026-10-13，微雪 ESP32-S3-Touch-LCD-3.5B，240 MHz、320×480 RGB565、QSPI DMA 经内部 RAM 暂存。使用同一诊断固件、同一普通表面、同一几何和颜色，每帧清屏并绘制 16 个矩形；测量 128 帧。没有改变公开图形 ABI 或新增 Scene/图集格式。

源码在 `tests/fixtures/graphics/`。individual 每条命令独立调用现有 aster_gfx_exec，batched 用一次调用提交同样的 17 条命令。两者每帧都完整重画，不能用“静态场景只画一次”虚增吞吐。该比较是同一固件中的调用方式优化，不是重写屏幕驱动后的性能变化，也不是 launcher 的直接测量。

## 实测

第一版诊断镜像各做三轮：individual 的平均绘制+提交耗时为 362.41、362.39、362.34 ms/帧，batched 为 42.45、42.41、42.38 ms/帧。最终 RGB565 像素在全部六次截图中一致。

修正内存统计后的同一镜像重新比较：individual 362.52 ms/帧，batched 42.48 ms/帧，约快 8.53 倍；实际平均帧间隔分别 363.75 和 43.68 ms（约 2.75 与 22.90 帧/秒）。APP 请求 fps=60 不代表实际达到 60。

gfx_exec 从 17 次/帧减少到 1 次/帧；128 帧合计 2176 与 128 次。加上该场景每帧两次 time_now_ms，稳态 APP ABI 调用数为 2432 与 384（不计 init/log/stop），不能把 gfx 调用减少 17 倍说成全部 ABI 调用减少 17 倍。

修正镜像中 batched 的两份 64-call native 样本：raster 8513/8499 μs，flush 33917/33902 μs。individual 每份也是按 64 **调用**平均，不是 64 完整帧：34 个样本每次仅一条命令，需要结合每帧 17 次才能解释。此处 flush 是 Host 提交/等待路径，包含缓冲转换与等待，不等于仪器测得的纯 QSPI 线时间；异步路径和日志开销可能影响数值。没有分位数级真机帧延迟或屏幕 TE 测量。

两个模式修正后的 Guest 线性内存峰值均为 **544768 字节**，没有为合批额外增加 Guest 峰值。系统堆和 PSRAM 最大空闲块的前/运行中/后快照保存在原始 JSON；短期比较未证明所有资源长期完全回到基线，长期稳定性仍按 P0 清单验收。

## 修正的统计缺陷

原 app stats 使用 wasm_runtime_get_mem_alloc_info，却没有检查返回值。当前固件使用 Alloc_With_Allocator，而该查询只支持 pool 模式，因此可能读取未初始化的结构体；旧报告的 peak=55 不代表真实内存。

现在使用 wasm_runtime_get_app_addr_range 查询有效的 Guest 线性范围，失败或超出 u32 时不更新统计。该数字是可访问线性缓冲的字节峰值，不是 malloc 活跃字节，也不包括 AOT 代码、字体、图形前缓冲或所有系统 PSRAM。Host C 回归覆盖范围增长、查询失败、反向范围和 4 GiB 溢出；微雪确认基准为 544768、Snake 为 286720 字节。ABI 布局和导入不变。

## 复现

在三个仓库父目录构建两个 AOT APP：

```bash
python aster-sdk/tools/build.py aster-firmware/tests/fixtures/graphics/individual aster-firmware/tests/fixtures/graphics/batched --target xtensa --aot
```

在独立微雪配置中开启 CONFIG_ASTER_GFX_TIMING=y 和 CONFIG_ASTER_DISP_TIMING=y，保留 USB shell；构建并确认刷入诊断镜像。使用同一块板和同一配置：

```bash
python aster-firmware/tools/graphics_benchmark.py --sdk aster-sdk --port /dev/ttyACM0 --firmware aster-firmware/build/firmware/zephyr/zephyr.bin --output aster-firmware/build/graphics-measurement --runs 3
```

命令只创建 test.p2.gfx.individual/batched，已有同 ID 默认拒绝覆盖。--resume 仅用于确认属于自己的中断测试，会停止这两个测试 APP。运行中的普通 APP 需先退出。脚本检查 AOT 启动、128 帧和错误数、截图 CRC/像素、Guest 统计及系统内存；停止测试、卸载测试 APP 并删除测试包。测试异常时保留证据，确认测试 APP 状态后处理；结束后刷回关闭 profiler 的正式配置。

## 结论与范围

这个场景能够被现有批处理表达，主要收益来自减少提交/等待次数；目前没有证据需要新增图形 ABI。APP 应优先采用 SDK 已有的 aster_ge_* ring，控制溢出时的中途提交，并保留完整 redraw/脏矩形的正确性约束。

原始结果、截图和回放证据见 [validation/p2-2026-10-13/](validation/p2-2026-10-13/)，corrected-memory/ 保存修正统计后同一固件的前后对比，production.json 记录正式固件恢复和 13 个 AOT APP 保留。最后已关闭 profiler/故障注入、清理测试 APP，Snake 启停正常。

本轮只验证微雪上的矩形全帧场景与最终像素；未逐帧检查扫描中的瞬态画面。中文文本、抗锯齿、bitmap/缓存、脏矩形、launcher 动画和 LCKFB 应另建固定场景；不能把 8.53 倍外推到所有 APP。LCKFB 已完成下述独立实机补测，仍未覆盖所有图形与 launcher 场景。Windows 模拟器新增文件清单已调整，原生便携打包/运行仍待验收。模拟器可重复的状态/截图不替代上述硬件检查。

## LCKFB 纯配置补测（2026-10-13）

LCKFB ESP32-S3，240×320 RGB565，明确选择 CONFIG_ASTER_HW_LCKFB_S3=y。同一固定场景各运行三轮：individual 平均绘制耗时为 43.97、43.95、43.98 ms/帧，batched 为 28.71、28.73、28.67 ms/帧；三轮均值为 43.97 与 28.70 ms/帧，约快 1.53 倍，耗时降低约 34.7%。实际平均帧间隔为 45.26 与 29.92 ms（约 22.10 与 33.42 帧/秒）。

全部六份截图像素相同，两种模式 Guest 线性内存峰值均为 544768 字节。原始数据、镜像/配置哈希和截图见 [LCKFB 图形证据](validation/lckfb-2026-10-13/graphics/graphics.json)。这与微雪的分辨率、显示通路不同，不将 8.53 倍结果外推至立创板。

首轮共用 build/ 根目录残留的微雪配置被 Zephyr 自动合并，结果已作废并移入 superseded-mixed-config/；上述数字均来自移除残留配置后重新刷入的纯 LCKFB 图形镜像。构建工具现拒绝未明确选择的 build/ 根目录配置碎片，并检查实际板型选择。
