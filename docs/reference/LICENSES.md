# 许可与第三方组件清单

ASTER 自研代码采用 **MIT License**（见仓库根 [LICENSE](../LICENSE)，
Copyright (c) 2026 mjgame）。所有自研源文件头部带
`SPDX-License-Identifier: MIT`。本文件说明**随仓库携带的第三方（vendored）
组件保持各自上游许可**，以及使用/再分发时的注意事项。

## 1. 自研部分（MIT）

`abi/`、`firmware/`（除 `modules/`）、`sdk/`（除 `toolchain/`、`build/`、
`dist/`）、`docs/`（除本文件中提及的上游文本）——ASTER 系统层、参考端口、
SDK/示例应用、PC 工具与文档。

用 ASTER SDK 编译出的 **App 二进制不受本仓库许可传染**：MIT 无 copyleft，
`sdk/lib` 静态链入 App 的 crt/printf/gfx 助手不需要任何附加义务；App 作者
可自由选择自己代码的许可。

## 2. Vendored 第三方组件

| 路径 | 组件 | 上游许可 | 说明 |
|---|---|---|---|
| `firmware/modules/wamr/` | WebAssembly Micro Runtime 2.4.3 | Apache-2.0 **WITH LLVM-exception** | 完整保留上游 `LICENSE` 与 `ATTRIBUTIONS.md`；ASTER 使用其 Classic Interpreter + Xtensa AOT |
| `firmware/modules/minimp3/` | minimp3（`minimp3.h`） | CC0 1.0 | 参考端口 MP3 解码；仅 `platform/esp32s3` 引用，系统层禁止 include |
| `firmware/modules/gc0308/` | GC0308 DVP 相机驱动（Zephyr module，5 文件） | Apache-2.0（**有意保留**） | 本仓库自研的驱动模块，但寄存器默认表参考 `espressif/esp32-camera`（Apache-2.0），故整体以 Apache-2.0 标识发布（Apache-2.0 与 MIT 双向兼容），不并入 MIT |
| `sdk/toolchain/wasi-sdk-24.0-x86_64-linux/` | wasi-sdk 24.0（clang-18/wasm-ld 等，已剔除 sysroot） | Apache-2.0 WITH LLVM-exception（含 picolibc 等 MIT/BSD 组件） | 构建工具，不进入任何发布二进制；见上游 <https://github.com/WebAssembly/wasi-sdk> |
| `sdk/toolchain/wamrc` | WAMR AOT 编译器（xtensa LLVM 分支构建） | Apache-2.0 WITH LLVM-exception | 由 `firmware/modules/wamr/wamr-compiler` 源码构建 |
| `sdk/toolchain/binaryen/bin/wasm-opt` | binaryen 130 | Apache-2.0 WITH LLVM-exception | 可选体积优化；上游 <https://github.com/WebAssembly/binaryen> |
| `sdk/vendor/lvgl/` | LVGL 9.5.0（仅保留头文件与所需 .c，`prune_src.sh` 可复现） | MIT | 系统 UI 图形库。ABI 3.0 起只由固件侧 LVGL 底座使用（桌面 launcher + 字体池 + screenshot），guest app 不再链接。上游 <https://lvgl.io>，`LICENCE.txt` 原样携带 |
| `sdk/assets/icons/lucide/` | Lucide 图标（离线渲染为 ASTI `.bin`，作 app 包根 `icon.bin` 桌面瓦片） | ISC（MIT 兼容） | 仅取矢量图形，逐文件上游头注明；`NOTICE` 随目录携带 |
| `firmware/modules/prettypoly/pretty-poly.h` | pretty-poly — 超采样多边形光栅器（tile 式，X1/X4/X16 抗锯齿） | MIT | 本地打补丁：分配钩子改用 2 KB bump arena、`qsort`→插入排序、`M_PI` 兜底（picolibc）。上游 <https://github.com/lowfatcode/pretty-poly> |
| `sdk/assets/fonts/DroidSansFallbackFull.ttf` | Android Droid Sans Fallback（**仅 CJK + PUA，无 ASCII**，是合成基底） | Apache-2.0 | 许可原文 `DroidSansFallbackFull.LICENSE`；上游 <https://android.googlesource.com/platform/frameworks/base/+/master/data/fonts/> |
| `sdk/assets/fonts/ui.ttf`（构建产物，不入 git） | 全局唯一界面字体：`build_ui_ttf.py` 将 DejaVu Sans 拉丁字形嫁接进 Droid，推送为 `/SD:/aster/fonts/ui.ttf`，LVGL tiny_ttf 流式渲染（cpfont 兜底与码点来源）；系统桌面与 guest app 的 gfx 文字共用 | Apache-2.0 + Bitstream Vera（字形级混合） | DejaVu 义务见 `sdk/assets/fonts/NOTICE` |
| `sdk/assets/fonts/ui_<px>.cpfont`（构建产物，不入 git） | cpfont v4 预烘焙位图：`build_cpfont.py` 从 `ui.ttf` 烘焙，推送为 `/SD:/aster/fonts/ui_<px>.cpfont` | Apache-2.0 + Bitstream Vera（字形级混合，随 `ui.ttf`） | 格式机制参考 CrossPoint Reader CJK（MIT，**独立实现未复制代码**，上游 (github.com/CrossPoint-CJK/crosspoint-reader-cjk)），规格见 `docs/cpfont-notes.md` |

注：社区参考包 `esp32s3_ui_kit`（GPL-3.0）仅用作**布局与主题观感参考**，
其代码与位图资源未进入本仓库；图标改由 ISC 的 Lucide 重绘，主题为本仓库
自实现。

注：**cpfont v4** 是 UI 字体主路径的预烘焙 4bpp 位图格式。其格式与设备端加载
机制参考 CrossPoint Reader CJK（**MIT**，
(github.com/CrossPoint-CJK/crosspoint-reader-cjk)），ASTER 为**独立实现**——生成器
`build_cpfont.py` 与加载器 `font_sd.c` 均按同一 on-disk 布局自写，**未复制上游
代码**，源文件头以 `SPDX-License-Identifier: MIT` 标识，属 §1 自研部分；规格与
移植笔记见 `docs/cpfont-notes.md`。字形版权随源字体 `ui.ttf`（见上表）。

`sdk/toolchain/LICENSE` 随仓库携带上述工具的 Apache-2.0 WITH LLVM-exception
许可文本（分发二进制所需）；WAMR 的文本与归属清单在
`firmware/modules/wamr/{LICENSE,ATTRIBUTIONS.md}`。

**依赖但不在仓库内**：Zephyr RTOS 与 Espressif HAL（均为 Apache-2.0）经
west workspace 引入，见 `west.yml` / 各上游仓库。

## 3. 义务与再分发

- MIT：保留版权与许可声明即可（仓库根 LICENSE + 各文件 SPDX 头满足）。
- CC0（minimp3）：公共领域，无附加义务；保留上游声明即可。
- Apache-2.0 组件：本仓库已按 §4 要求**原样携带上游 LICENSE/NOTICE**（wamr
  目录），修改处不冒认上游；`LLVM-exception` 免除"编译产物嵌入运行库"的分发
  条件，因此用自带工具链编出的 App/固件二进制可自由分发。
- 若你 fork/再分发本仓库，请连同 `firmware/modules/`、`sdk/toolchain/` 内的
  上游许可文件一起保留，不要删除 SPDX 头或替换其标识。
