# 许可与第三方组件清单

ASTER 自研代码采用 **MIT License**（见仓库根 [LICENSE](../../LICENSE)，
Copyright (c) 2026 mjgame）。本文件说明**随仓库携带的第三方（vendored）
组件保持各自上游许可**，以及使用/再分发时的注意事项。

## 1. 自研部分（MIT）

固件的系统、接口与工具分布在 `contract/`、`common/`、`systems/`、`platform/`、`boards/` 和 `tools/`。各文件的版权与来源说明随源码保留。应用开发工具链位于独立 SDK，应用与游戏另有各自的素材和上游许可。[固件来源声明](../../NOTICE.md) · [SDK 来源声明](https://github.com/hello-myj/aster-sdk/blob/main/NOTICE.md)

## 2. Vendored 第三方组件

| 路径 | 组件 | 上游许可 | 说明 |
|---|---|---|---|
| `modules/wamr/` | WebAssembly Micro Runtime 2.4.3 | Apache-2.0 **WITH LLVM-exception** | 完整保留上游 `LICENSE` 与 `ATTRIBUTIONS.md`；ASTER 使用其按 CPU 架构编译的 AOT 运行时 |
| `modules/minimp3/` | minimp3（`minimp3.h`） | CC0 1.0 | 参考端口 MP3 解码；用于支持 MP3 的固件配置 |
| `modules/gc0308/` | GC0308 DVP 相机驱动（Zephyr module，5 文件） | Apache-2.0（**有意保留**） | 本仓库自研的驱动模块，但寄存器默认表参考 `espressif/esp32-camera`（Apache-2.0），故整体以 Apache-2.0 标识发布（Apache-2.0 与 MIT 双向兼容），不并入 MIT |
| `third_party/lvgl/` | LVGL 9.5.0（固件使用的源码快照） | MIT | 系统 UI 图形库。ABI 3.0 起只由固件侧 LVGL 底座使用（桌面 launcher + 字体池 + screenshot），guest app 不再链接。上游 <https://lvgl.io>，`LICENCE.txt` 原样携带 |
| `modules/prettypoly/pretty-poly.h` | pretty-poly — 超采样多边形光栅器（tile 式，X1/X4/X16 抗锯齿） | MIT | 本地打补丁：分配钩子改用 2 KB bump arena、`qsort`→插入排序、`M_PI` 兜底（picolibc）。上游 <https://github.com/lowfatcode/pretty-poly> |
| `resources/fonts/DroidSansFallbackFull.ttf` | Android Droid Sans Fallback（**仅 CJK + PUA，无 ASCII**，是合成基底） | Apache-2.0 | 许可原文 `DroidSansFallbackFull.LICENSE`；上游 <https://android.googlesource.com/platform/frameworks/base/+/master/data/fonts/> |
| `resources/fonts/ui.ttf` | 生成 CPFont 的合成源字体 | Droid 和 DejaVu 对应许可 | [字体来源与说明](../../resources/fonts/NOTICE) |
| `resources/fonts/ui_<px>.cpfont` | 十个原生字号的系统位图字库 | 继承字形的源字体许可 | [字号清单](../../resources/fonts/faces.json) |

注：社区参考包 `esp32s3_ui_kit`（GPL-3.0）仅用作**布局与主题观感参考**，
其代码与位图资源未进入本仓库；图标改由 ISC 的 Lucide 重绘，主题为本仓库
自实现。

注：**cpfont v4** 是 UI 字体主路径的预烘焙 4bpp 位图格式。其格式与设备端加载
机制参考 CrossPoint Reader CJK（**MIT**，
(github.com/CrossPoint-CJK/crosspoint-reader-cjk)），ASTER 为**独立实现**——生成器
`build_cpfont.py` 与加载器 `font_sd.c` 均按同一 on-disk 布局自写，**未复制上游
代码**，源文件头以 `SPDX-License-Identifier: MIT` 标识，属 §1 自研部分；当前的字号和构建说明见 [字体 NOTICE](../../resources/fonts/NOTICE)。字形版权随源字体 `ui.ttf`（见上表）。

WAMR 的许可文本与归属清单位于 [LICENSE](../../modules/wamr/LICENSE) 和 [ATTRIBUTIONS](../../modules/wamr/ATTRIBUTIONS.md)。可选 WAV、FLAC 和 MP3 媒体解码器和图像解码器分别保留[音频来源说明](../../third_party/dr_libs/README.aster.md)和[图像组件许可证](../../modules/imgdec/vendor/LICENSE)。

**依赖但不在仓库内**：Zephyr RTOS 与 Espressif HAL（均为 Apache-2.0）经
west workspace 引入，见 `west.yml` / 各上游仓库。

## 3. 发布时核对的文件

固件仓库的 [LICENSE](../../LICENSE) 与 [NOTICE](../../NOTICE.md) 记录项目的许可与来源。打包时还应核对实际包含的代码、工具和字体，并随包保留各组件适用的版权和许可文件。WAMR 的 LLVM 例外条款见其[上游许可原文](../../modules/wamr/LICENSE)。应用自带的游戏数据、字体或音乐另见对应应用的来源声明。
