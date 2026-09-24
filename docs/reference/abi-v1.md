# ASTER ABI Specification

Status: **3.2 is the current release** (additive over the 3.0 breaking release) —
the wasm-side widget runtime (`aster/ui.h`, `ui.bin`, `ASTER_EVENT_UI`) was deleted
outright; guest apps now draw only through the gfx engine
([gfx-engine.md](gfx-engine.md)). Additive changes bump minor; breaking changes
require a major bump. The changelog below stays for history; symbols removed in
3.0 (and 2.0) are listed in their entry and no longer appear in the §3 tables.

> **3.2（增量，2026-09）**：新增 `aster_audio_get_info`（`audio.h`），**并删除**
> `ASTER_AUDIO_RATE` / `_CHANNELS` / `_BITS` 三个宏（源码级破坏，但**报文兼容**
> —— 树内与所有已发布的 app 都没引用过它们）。理由：这三个数是**板级事实**
> （由 codec 与 I2S 时钟决定），却被写进 app 编译一次、装到任意板子上的头文件
> 里 —— 于是它们在**除了那一块板以外**的每一块板上都是错的，而且是**静默**错：
> 采样率写错不会报错，只会以错误的速度录放。同一类错误在固件侧也修了：运行时
> 曾用 `ASTER_PLAT_CAM_W/H` / `ASTER_PLAT_AUDIO_*`（同样是一个板子的数）校验 app
> 缓冲区，换传感器/codec 会把所有调用判 `EINVAL`；现在改为向 port 询问
> （`aster_plat_cam_info` / `aster_plat_audio_info`），数只存在于板子自己的
> Kconfig。`aster_audio_info_t` 是 2.0 就留下的类型，此行兑现它。
>
> **同一轮 bump 的另一半**：`aster_display_info_t` 新增 `app_width` /
> `app_height` —— **surface 按这个尺寸创建、触摸按这个尺寸上报**。`width` /
> `height` 的语义**不变**，仍是「玻璃的逻辑像素」。两者相等即 1:1；不等则
> 该板把 app 空间缩小了整数倍（`CONFIG_ASTER_APP_SPACE_DIV`，见
> [`../firmware/Kconfig`] 与 `docs/verification.md` 75 号条目），app 空间 =
> 面板 / DIV，两轴同除，放大是干净的最近邻整数倍。
> **app 必须按 `app_width`/`app_height` 排版，不能按 `width`/`height`**：
> surface 请求是对前者校验的，所以写死参考板数字（320×240）的 app 在带 DIV 的
> 板上会被**明确拒绝**（响亮失败）；而若不校验，它会安静地渲染到屏幕外。
> 这一条对所有既有 app 都是行为变更——在 3.2 之前，树内**没有** app 读
> `app_width`，全都隐式假定 app 空间 == 面板，而这个假定在参考板上恰好成立。
> 今天唯一读它的是 `sdk/apps/apitest`（校验上面那两条性质），它的
> `app.toml` 因此标 `abi_min = "3.2"`。
> 用到它的包 `abi_min="3.2"`；3.2 设备无缝跑 3.0/3.1 包。
> **§3 音频表里 `size` 必须是 `channels * bits/8` 的倍数的说法，`channels`
> 与 `bits` 现在从 `aster_audio_get_info` 取，不再假定为 2 和 16。**

> **3.1（增量，2026-09）**：新增 `aster_gfx_fonts_pending`（`gfx.h`），让**只在
> 事件驱动下重绘**的 app 知道「现在该重画了」。背景：烤字字形位图由 SD 异步
> 补入，未就位的码点先画成尺寸正确的**空白框**，等位图落地才补全。持续重绘的
> app（游戏、每帧 `ASTER_EVENT_FRAME` 都画的界面）不受影响；而「按键才画一次」
> 的阅读器/播放器不会再自绘，空白框就会一直留在屏上——这类 app 把该调用并进
> 重绘条件即可：
>
> ```c
> if (dirty || aster_gfx_fonts_pending()) { render(); }
> ```
>
> 返回 1 表示自上次绘制以来有字形落地（或有烤字号刚转 READY），0 表示流水线
> 已静止。语义是**电平**不是边沿：读多少次结果都一样，只有 `aster_gfx_exec`
> 真正出图才清零，所以多调用一次不会把信号「吃掉」。调用本身会推进取字形/
> 提交两级流水（不自行泵它的 app 否则会把已取回的位图滞留在暂存区），不阻塞、
> 不返回错误。用到它的包 `abi_min="3.1"`；3.1 设备无缝跑 3.0 包。
> 树内 `sdk/apps/reader` 与 `sdk/apps/player` 已改用。

> **3.0（破坏性，2026-09）**：**删除 wasm 侧控件运行时**。`aster/ui.h`（`page_show` /
> `set_text` / `set_visible` / `set_value` / `bind_click` / `commit` / `list_*` /
> `theme` / `set_file` / `font_open`）、`ASTER_EVENT_UI` 与 `aster_event_t.u.ui`
> 联合体、包内 `ui.bin`（UIB1）段、`sdk/tools/ui_compile.py` 全部移除，不留兼容桩。
> Guest app 的界面只有一条路径：自己持有 RGB565 surface + **gfx 命令引擎**
> （[gfx-engine.md](gfx-engine.md)）。系统桌面不走 app ABI（固件内置 LVGL
> launcher），不受影响。
> 包格式：`.mja` 头 56..63 字节改为保留位（必须 0），`flags` bit1 保留；
> `CONFIG_ASTER_UI_RUNTIME` → `CONFIG_ASTER_LVGL`（语义变为「LVGL 底座：
> 桌面 + 字体池 + screenshot」）。删除的示例 app：`clock.ui` / `reader.ui` /
> `filer.ui` / `flush.ui` / `music`（阅读器与播放器界面将用绘图引擎重做）。
> 安装门禁按 major 相等放行：**1.x / 2.x 包在 3.0 设备上被 `ASTER_EABI` 拒绝**。
> 详见 [ui-runtime.md](ui-runtime.md)（墓碑页）。

> **2.0（破坏性，2026-09）**：移除 6 个冗余 import，不留兼容桩：
> `aster_audio_get_info`（格式固定为编译期宏 `ASTER_AUDIO_*`）、`aster_audio_play` /
> `aster_audio_start`（播放一律走后台会话 `aster_audio_start_file`；`record` 保留）、
> `aster_ui_list_add`（`list_add_ex` 空 icon/subtitle 即等价）、`aster_ui_theme`
> （主题归固件 native launcher）、`aster_system_get_info`（ABI 查询用
> `aster_system_get_abi_version`，能力用 `aster_system_has_feature`）。
> 类型 `aster_audio_info_t` / `aster_system_info_t` 保留（宿主内部仍用）。
> 安装门禁按 major 相等放行：**1.x 包在 2.0 设备上被 `ASTER_EABI` 拒绝**，
> 反之 2.0 包在旧固件上 import 链接失败。全部在树示例已重编为 2.0。
>
> **2.1（语义，非破坏）**：字体栈统一到 **LVGL TinyTTF + 每字号 LRU 字形缓存 +
> SD 单 TTF**。所有字形来自 `/SD:/aster/fonts/ui.ttf`（`S:/aster/fonts/ui.ttf`），
> 固件按字号懒建 tiny_ttf 实例（字号池上限 8，缺文件回退 placeholder 不崩）。
> `aster_ui_font_open` 符号**保留但改为 no-op**（恒 `ASTER_OK`，`path` 忽略），故 2.0
> 包继续链接运行；`aster_ui_set_value` 对 `font_src=sd` label 仍是换阅读字号
> （落到 14/16/18/20），与字体来源无关。包内 MJXF 子集、SD `reader_<px>.bin`、
> rodata 烤字、动态 sysfont 四条旧路径全部移除，App 不再打任何字体资源。
> 见 [ui-runtime.md](ui-runtime.md)。
>
> **2.2（增量）**：GFX 命令引擎 —— app 端把绘制意图编码成定长 24B 命令环，
> 一帧一次 `aster_gfx_exec` 提交，固件光栅器直接画进 app 的 RGB565 surface
> 并推脏区并集（CLEAR/矩形/圆角/AA 线/椭圆/三角/渐变/UTF-8 文字/精灵 blit）。
> 文字复用 cpfont 常驻度量 + tiny_ttf 兜底（白框异步协议），精灵走固件
> imgdec + 6 槽 LRU PSRAM 缓存（`aster_gfx_bmp_load/free`）。新增 4 个 import，
> 无破坏；用到新符号的包 `abi_min="2.2"`。头 `gfx.h`，详见
> [gfx-engine.md](gfx-engine.md)，演示 `sdk/apps/gfxshow`。
>
> **1.1（增量）**：键盘/按键链路 —— `aster_input_keys_state()`、
> `ASTER_EVENT_KEY`（`u.key.code/pressed`）、`input_get_info().kinds` 报告
> `ASTER_INPUT_KEYS`、`feature_flags` 的 `ASTER_FEATURE_KEYBOARD`。老设备
> （1.0 runtime）上 1.1 应用因缺少 keyboard 能力位而 `requires` 失败或被忽略；
> 1.1 设备可无缝运行 1.0 应用。下文相应小节以 **[1.1]** 标注。

> **1.2（增量）**：权限执法 + 应用管理面 —— manifest 的 `permissions`
> 从"仅记录"变为**硬门禁**（未声明即 `ASTER_EPERM`），新增 `appmgr.h`
> （`aster_appmgr_count/list/start/uninstall`）供系统 UI 使用，以及开机自启
> 画像 `CONFIG_ASTER_SYSTEM_UI`。**破坏性**：1.2 runtime 上未声明所用权限的
> 1.0/1.1 包会被拒（1.0/1.1 runtime 上 1.2 应用因符号缺失而启动失败，
> 属预期的能力协商）。全部在树示例已补齐声明。

> **1.3（增量）**：运动传感器 + 相机拉取面 —— 新增 `sensor.h`
> （`aster_accel_read/aster_gyro_read`，SI 单位，按 `ASTER_FEATURE_ACCELEROMETER/
> _GYROSCOPE` 能力门控，无传感器返回 `ASTER_ENODEV`）与 `camera.h`
> （`aster_cam_info/aster_cam_read`，RGB565 帧，按 `ASTER_FEATURE_CAMERA` 门控）。
> 纯能力查询面，**不引入新权限**（传感器/相机与 display/input 同属硬件事实）。
> 向后兼容：1.3 设备仍可运行 1.0–1.2 包；用到新符号的包需 `abi_min="1.3"`，
> 旧固件安装期即拒。下文 §6.1、§6.2。

> **1.4（增量）**：脏区刷屏 `aster_surface_flush()`（`surface.h`）—— 把矩形
> 脏区从应用线性内存直推面板，无需整帧暂存拷贝。这是 LVGL PARTIAL 渲染
> 模式的配套：1 Hz 时钟跳字只刷 ~60×60 px，而不是 240×320 整帧（实测
> 桌面 2 fps → ~23 fps）。仅支持 RGB565 surface（565 是唯一全平台原生
> 格式，快速路径不做转换）。用到它的包需 `abi_min="1.4"`，旧固件安装期
> 即拒。下文 §3 display/surface。
>
> **1.5（增量）**：Native UI Runtime —— `aster/ui.h`（`page_show` / `set_text` /
> `set_visible` / `set_value` / `bind_click` / `commit` / `list_*` / `theme`）
> 与 `ASTER_EVENT_UI`。界面由包内 `ui.bin` 描述、固件 LVGL 渲染；wasm 禁止再
> 链接 LVGL。用到这些符号的包需 `abi_min="1.5"`。见 [ui-runtime.md](ui-runtime.md)。
>
> **1.6（增量）**：录音/播放拉取面 —— `aster/audio.h`（`aster_audio_get_info` /
> `aster_audio_play` / `aster_audio_record`）。V1 固定 **16 kHz / 16-bit / 立体声
> s16le 交错 PCM**，阻塞整段缓冲，无流句柄、无异步事件、无新权限；按
> `ASTER_FEATURE_AUDIO_OUTPUT` / `ASTER_FEATURE_AUDIO_INPUT` 门控，缺硬件返回
> `ASTER_ENODEV`。用到这些符号的包需 `abi_min="1.6"`。下文 §6.3。
>
> **1.7（增量）**：播放音量 + 后台会话 —— `aster_audio_set_volume` /
> `aster_audio_get_volume`（0–100，0=静音）以及 `aster_audio_start` / `pause` /
> `resume` / `stop` / `get_state`。`start` 把 PCM 拷进宿主缓冲后立即返回，
> worker 可继续跑；阻塞 `play`/`record` 仍保留。会话占用时 `play`/`record`/
> `beep` 返回 `ASTER_EBUSY`。无混音器、无格式协商、无新权限。用到 1.7 符号的包需
> `abi_min="1.7"`。下文 §6.3。
>
> **1.8（增量）**：同一套 `aster_audio_play` / `aster_audio_start` 也可传入
> MPEG-1/2 Layer III（可选 ID3）。固件解码并重采样到原生 16 kHz 立体声 PCM，
> 无新符号、无新权限。PCM 契约不变。下文 §6.3。
>
> **1.9（增量）**：`aster_audio_start_file(path)` 按沙箱路径流式后台播放
> （WAV/PCM/MP3），不受 `start()` 的 2 MiB 拷贝上限。路径 `/music/…` 映射到
> 共享曲库 `/SD:/music`（只读，需 `fs` 权限）。下文 §6.3、fs。
>
> **1.10（增量）**：`aster_ui_font_open` 打开 SD 阅读字库。优先把
> `/sd/aster/fonts/reader_<px>.bin`（MJXF v2，整份进 PSRAM）挂到
> `"font_src": "sd"` 的 label；没有这份位图时才 TinyTTF 流式读 `reader.ttf`。
> 空路径 = `/sd/aster/fonts/reader_16.bin`。需 manifest `fs` + `sd`。
> `aster_ui_set_value(id, px)` 换字号（固件落到 14/16/18/20），不另增符号。
> 用到该符号的包需 `abi_min="1.10"`。见 [ui-runtime.md](ui-runtime.md)。
> *（历史：2.1 起 MJXF 装载路径移除，`font_open` 变 no-op，见顶部 2.1 条目。）*
>
> **1.11（增量）**：`aster_ui_set_file` 由固件按 label 宽高排一页文本、识别 UTF-8/GB2312、
> 可选去掉 LRC 时间戳，再 `set_text`。上划/下划在固件翻页。把 17 KB 码表和大缓冲留在宿主，避免 wasm/AOT
> 栈溢出。用到该符号的包需 `abi_min="1.11"`。见 [ui-runtime.md](ui-runtime.md)。
>
> **1.13（增量）**：`aster_audio_seek(msec)` 跳转后台播放会话；`get_state` v2
> 增加 `msec_done` / `msec_total`。`aster_audio_get_lyrics` 返回 UTF-8 歌词
> （优先 MP3 ID3 USLT/SYLT，否则同目录 `.lrc`）。用到新符号的包需
> `abi_min="1.13"`。
>
> **1.14（增量）**：图片解码预览。`aster_img_probe(path)` 只读头部给出格式/
> 尺寸/fit 估计；`aster_img_render(path, args)` 把 BMP/JPG/PNG 流式解码成
> 小端 RGB565，写入应用自己的缓冲（一般是 surface back-buffer），再用
> `aster_surface_flush` 上屏。解码期内存与文件大小、原图尺寸均解耦（宿主侧
> 固定几百 KB PSRAM）。两个调用都阻塞应用 worker（大图 PNG 数秒），应在
> 触摸/滑动事件里驱动而不是每帧。路径按 fs 沙箱映射，需 manifest `fs`。
> 用到新符号的包需 `abi_min="1.14"`。见 `aster.gallery` 示例。

## 1. Module & calling convention

- Apps are WebAssembly core modules, target `wasm32-unknown-unknown` (generic wasm).
- All ASTER host functions are imported from module **`env`** with symbol names
  `aster_*`. The canonical declarations live in `abi/include/aster/*.h`
  (single source of truth; `sdk/include/aster` is a sync copy).
- On wasm32, C struct pointers are linear-memory offsets (u32). The Runtime
  validates every `(ptr,len)` against the module's memory before use; a failing
  check returns `ASTER_EINVAL`, never a crash of the host.
- Every ABI struct begins with `{ uint32_t size; uint32_t version; }`.
  Callers fill in `size = sizeof(T)` and `version = 1`. The Runtime uses
  `min(size, sizeof(runtime struct))` so future field additions stay
  backward compatible.

## 2. Exports required from the app

| export | signature | required |
|---|---|---|
| `aster_app_init` | `()->i32` | yes (0 = accept launch) |
| `aster_app_event` | `(i32)->()` (event struct offset) | yes |
| `aster_app_start` | `()->()` | optional |
| `aster_app_pause` | `()->()` | optional |
| `aster_app_resume` | `()->()` | optional |
| `aster_app_stop` | `()->()` | optional |
| `aster_app_destroy` | `()->()` | optional |

The Runtime drives apps purely through the exports above (no `main`). The SDK
`crt.c` supplies weak defaults for every lifecycle export except `aster_app_init`
and `aster_app_event` (which apps must provide), so an app only implements what it
uses. `crt.c` is linked `--whole-archive` so the weak stubs are always present.

The SDK `crt.c` also emits an exported **data symbol `__aster_abi`** = ASTER_ABI_VERSION.
V1 enforces ABI compatibility through the **package manifest** (`abi.min` major
must equal the device major; see `mja-format.md` and `aster_mja.c`), not by reading
this wasm data symbol — WAMR's embed API exposes no global/data lookup in the
pinned version, so `__aster_abi` is kept as a forward-compat marker that tooling
and future loaders can assert on.

## 3. Imports (native functions)

All return `i32` error codes (`ASTER_OK` = 0) unless noted. Signatures use WAMR
native symbol syntax (`$` = pointer+offset arg).

### system
| symbol | signature (wasm) | notes |
|---|---|---|
| `aster_system_has_feature` | `(i32)i32` | feature bit -> 0/1 |
| `aster_system_get_abi_version` | `()i32` | packed 0x00MMmmPP |

(`aster_system_get_info` was removed in 2.0; the `aster_system_info_t` type stays
for host-internal use.)

### app/log/time
| symbol | signature | notes |
|---|---|---|
| `aster_log` | `(i32,i32,i32)` | level, msg ptr, len |
| `aster_app_exit` | `(i32)` | exit code; terminates app |
| `aster_app_set_framerate` | `(i32)i32` | default 30, clamp 1..60 |
| `aster_time_now_ms` | `()i32` | ms since app start |

### timer / input
| symbol | signature |
|---|---|
| `aster_timer_create` | `(i32,i32,i32,i32)i32` (period_ms, repeat, user, out_ptr) |
| `aster_timer_destroy` | `(i32)i32` |
| `aster_input_get_info` | `(i32,i32)i32` — `kinds` 位：`ASTER_INPUT_TOUCH` / **[1.1]** `ASTER_INPUT_KEYS` |
| `aster_input_touch` | `(i32,i32)i32` -> n points |
| **[1.1]** `aster_input_keys_state` | `(i32,i32)i32` -> 当前按住键的 code 数（code 数组写入 ptr） |

**[1.1] 按键模型**：`ASTER_EVENT_KEY` 传边沿流（`u.key.code`、`u.key.pressed`
1=按下/0=松开），code 为宿主原始 `INPUT_KEY_*`（Linux evdev 编号），ASTER 从不
重新编号——UI 层自行映射语义。`aster_input_keys_state()` 读瞬时按住集合，用于
丢边沿后重同步与配合定时器做 auto-repeat。平台侧接口对应
`aster_plat_input_keys_poll/state/available`（见 PORTING.md）。

### display / surface
| symbol | signature |
|---|---|
| `aster_display_count` | `()i32` |
| `aster_display_get_info` | `(i32,i32,i32)i32` (index, ptr, size) |
| `aster_surface_create` | `(i32,i32,i32)i32` (desc ptr, desc size, out ptr) |
| `aster_surface_destroy` | `(i32)i32` |
| `aster_surface_acquire` | `(i32,i32,i32)i32` (surface, buf ptr, buf size) |
| `aster_surface_present` | `(i32)i32` |
| `aster_surface_flush` **[1.4]** | `(i32,i32,i32,i32,i32,i32,i32)i32` (surface, src ptr, x, y, w, h, src_pitch) |

Surface semantics (V1): the buffer is allocated inside the app's wasm linear
memory (app heap) at create time, so `acquire` returns a stable pointer;
`present` copies + converts (RGB565 native; RGBA8888/BGRA8888 -> RGB565
conversion path provided) and writes the display through the Platform Layer.

**`ASTER_SURFACE_F_PANEL_CANVAS` (0x1, request in `aster_surface_desc_t.flags`)
is the exception to that paragraph** — and it is shipped and in use
(`sdk/apps/snake`, `sdk/apps/gfxshow`), so it belongs here rather than
only in [gfx-engine.md](gfx-engine.md) §5. With it, the firmware does **not**
give the app a buffer: `acquire` returns **`buf.ptr == NULL`**, pixels live in
firmware-owned front buffers, and only the gfx engine can reach them. The app
gets back the whole 7.9 ms/frame full-frame byte-swap copy that a normal
surface pays on `present`.

The contract, in full:

- **You must repaint every pixel, every frame.** Two front buffers alternate,
  so a partial repaint leaves a two-frames-old residue, not a stale-but-sane
  image.
- **The firmware may silently ignore the request** (rotated panel, buffers
  unavailable, **or the board's app space is not the panel** — see the
  `app_width`/`app_height` note at the top: a canvas is the *glass* in panel
  byte order, which only makes sense when app space and glass are the same
  size, so a board with `CONFIG_ASTER_APP_SPACE_DIV > 1` always denies) and hand
  back an ordinary app-memory surface instead. So: **always read `buf.flags`
  after acquire** and take the `ptr == NULL` branch as a *granted* canvas, not
  a failure. The log line `surface: WxH panel-canvas requested -> granted` /
  `-> denied` is the firmware side of the same answer.
- Byte order of a canvas is the **panel's**, not the ABI's; the engine's
  encoder knows this (`out_pack()`), which is why hand-writing pixels into a
  canvas is not a supported path.

**[1.4] `aster_surface_flush`** streams a dirty rectangle straight from *any*
linear-memory buffer (`src` need not be the surface buffer — that is what
lets a partial-rendering toolkit push its small draw bands without a
full-frame staging copy). `src` holds `w×h` packed RGB565 rows with row
stride `src_pitch` pixels (0 = `w`); `x,y` are destination coords on the
surface. `ASTER_ENOTSUP` if the surface format is not RGB565; `ASTER_EINVAL` if
the region leaves the surface or the buffer window leaves linear memory.
The surface remains the bounds/format authority; rotation/byte-order/panel
mapping stay Platform Layer policy (see PORTING.md 显示旋转).
Zero-copy shared buffers are reserved for ABI 1.x (`present_buffer` will appear
as a new symbol, never changing existing ones). Note this is **not** the same
thing as `ASTER_SURFACE_F_PANEL_CANVAS` above: a panel canvas is zero-copy but
*one-way* (firmware-owned, engine-only writes), whereas a shared buffer would
be app-writable.

### ui **[1.5]** — **removed in 3.0**
Host-side LVGL renders `ui.bin`. Apps must not link LVGL. Clicks are queued as
`ASTER_EVENT_UI` (`u.ui.id`, `u.ui.kind` = `ASTER_UI_CLICK` / `ASTER_UI_VALUE` / `ASTER_UI_LONGPRESS`).
Packages that import these symbols need `abi_min="1.5"`. See [ui-runtime.md](ui-runtime.md).

> **3.0**: every symbol below, `ui.bin`, `ASTER_EVENT_UI` and `u.ui` are gone. Use
> the gfx engine instead — see [gfx-engine.md](gfx-engine.md).

| symbol | signature |
|---|---|
| `aster_ui_page_show` | `(i32,i32)i32` (id, len) |
| `aster_ui_set_text` | `(i32,i32,i32,i32)i32` (id, id_len, text, text_len) |
| `aster_ui_set_visible` | `(i32,i32,i32)i32` |
| `aster_ui_set_value` | `(i32,i32,i32)i32` |
| `aster_ui_bind_click` | `(i32,i32)i32` |
| `aster_ui_commit` | `()i32` |
| `aster_ui_list_clear` | `(i32,i32)i32` |
| `aster_ui_list_add_ex` | `(iiiiiiiiii)i32` list/item/icon/title/subtitle 各 (ptr,len)；icon、subtitle 可空 |
| **[1.10, no-op in 2.1]** `aster_ui_font_open` | `(i32,i32)i32`（2.1 起恒 `ASTER_OK`，参数忽略；字体全局来自 SD TTF） |
| **[1.11]** `aster_ui_set_file` | `(iiiiiii)i` id/path (ptr,len) + offset + max_raw + flags；flags bit0=LRC；返回已读字节 |
| **[1.12]** `aster_ui_file_offset` | `()i32` 当前页文件偏移；无打开文件 → `ASTER_ENOENT` |

`set_value` **[1.10]** 对 `font_src=sd` 的 label 表示阅读字号（固件落到 14/16/18/20）。自 2.1
起字体全局可用（SD 单 TTF + tiny_ttf 缓存），`aster_ui_font_open` 不再装载任何资源。
**[1.11]** `set_file` needs `fs`+`sd` and `abi_min="1.11"`.
**[1.12]** `file_offset` needs `abi_min="1.12"`.

### audio **[1.7]** / **[1.8]** / **[1.9]** / **[1.13]**（2.0 精简）
Blocking record + background playback, no mixer. Native format is fixed
(16 kHz / 16-bit / stereo s16le) by the `ASTER_AUDIO_*` macros.
**[1.8]** background playback also accepts MPEG Layer III (decoded in firmware).
**[1.9]** `aster_audio_start_file` streams a sandboxed path.
**[1.13]** `aster_audio_seek` / `aster_audio_get_lyrics`; `get_state` v2 clock fields.
Gated by capability bits; `start_file` also needs manifest `fs`. 2.0 removed
`aster_audio_get_info` / `aster_audio_play` / `aster_audio_start` (in-memory copies).

| symbol | signature |
|---|---|
| `aster_audio_record` | `(i32,i32)i32` (pcm ptr, size bytes) — blocks; `EBUSY` if a session is running |
| **[3.2]** `aster_audio_get_info` | `(i32,i32)i32` (info ptr, struct_size) — native PCM format. `size` < 8 (size+version) is `EINVAL`; a size between 8 and `sizeof(aster_audio_info_t)` is filled as far as it goes |
| **[1.7]** `aster_audio_set_volume` | `(i32)i32` (0..100, 0 = mute, 100 = 0 dB) |
| **[1.7]** `aster_audio_get_volume` | `()i32` — 0..100, or a negative error |
| **[1.9]** `aster_audio_start_file` | `(i32,i32)i32` (path ptr, len) — stream sandboxed path, background |
| **[1.7]** `aster_audio_pause` | `()i32` — `ESTATE` if idle |
| **[1.7]** `aster_audio_resume` | `()i32` — `ESTATE` if idle |
| **[1.7]** `aster_audio_stop` | `()i32` — OK if already idle |
| **[1.7]** `aster_audio_get_state` | `(i32,i32)i32` (state ptr, struct_size); **[1.13]** v2 adds msec_done/msec_total |
| **[1.13]** `aster_audio_seek` | `(i32)i32` (msec from start) — `ESTATE` if idle |
| **[1.13]** `aster_audio_get_lyrics` | `(i32,i32)i32` (buf ptr, size) — UTF-8 bytes copied, 0 if none |

For PCM, `size` must be a multiple of `channels * bits/8` (4 on V1). MP3 size
has no frame-multiple requirement. `ASTER_ENODEV` if the matching `audio-out` /
`audio-in` bit is absent; `ASTER_EINVAL` on a bad pointer or size; `ASTER_EIO` if
the codec/I2S path fails (or the MP3 bitstream cannot be decoded). Blocking
`record` occupies the app worker for the whole buffer — keep PCM bursts short
(tens of ms) on a frame callback; playback streams in the background.

### fs (sandboxed)
**[1.2]** 全部 `aster_fs_*` 需 manifest `permissions=["fs"]`，否则首个调用即 `ASTER_EPERM`。

| symbol | signature |
|---|---|
| `aster_fs_open` | `(i32,i32,i32,i32)i32` (path,len,flags,out) |
| `aster_fs_read` | `(i32,i32,i32)i32` |
| `aster_fs_write` | `(i32,i32,i32)i32` |
| `aster_fs_seek` | `(i32,i32,i32)i32` |
| `aster_fs_close` | `(i32)i32` |
| `aster_fs_mkdir` | `(i32,i32)i32` |
| `aster_fs_unlink` | `(i32,i32)i32` |
| `aster_fs_stat` | `(i32,i32,i32,i32)i32` |
| `aster_fs_opendir` | `(i32,i32,i32)i32` |
| `aster_fs_dir_next` | `(i32,i32,i32,i32)i32` (dir, name ptr, name max, is_dir out ptr) |
| `aster_fs_closedir` | `(i32)i32` |

App-visible path roots: `/data` (persistent), `/cache`, `/assets` (read-only
from package), **[1.9]** `/music` (shared library → `/SD:/music`, **read-only**:
list/stat/open-read/play; write/mkdir/unlink → `ASTER_EPERM`). Apps that also
declare `permissions=["sd"]` may use `/sd` → `/SD:` (and `/sd/foo` → `/SD:/foo`).
Without `sd`, `/sd` is `ASTER_EPERM` (same as a sandbox escape). `..` escaping is
rejected. Per-app paths map to `/SD:/aster/apps/<id>/data` etc. Firmware creates
`/SD:/music` at mount. There is no `aster_fs_rename`; shell `aster fs mv` is the
privileged rename path.

libc imports: WAMR is built with `LIBC_BUILTIN=1`, so the Runtime also
registers a string/memory subset under `env` (`memcpy`, `memmove`, `memset`,
`memcmp`, `memchr`, `strlen`, `strcpy`, `strncpy`, `strcmp`, `strncmp`,
`strchr`, `strstr`, `strspn`, `strcspn`, `atoi`, `strtol`, `strtoul`,
`malloc`/`calloc`/`realloc`/`free`, `abort`, and the `*printf`/`*puts`/`*putchar`
family). `app.wasm` is linked `--allow-undefined`, so clang's implicit
`memset`/`memcpy` calls resolve to these. **libm is not provided** — apps that
need math must supply their own (the SDK's `aster_gfx` uses integer tables; see
`apps/snake`) or the SDK will add a builtin math shim later.

### img **[1.14]**
**[1.14]** 图片解码预览。`aster_img_probe` / `aster_img_render` 需 manifest
`permissions=["fs"]`（路径与 `aster_fs_*` 同沙箱映射，`/sd/...` 需 `sd`）。
宿主流式解码 BMP/JPG/PNG → 小端 RGB565 写入应用缓冲；两个调用都阻塞应用
worker。输出后由应用 `aster_surface_flush` 上屏。

| symbol | signature |
|---|---|
| `aster_img_probe` | `(i32,i32,i32,i32)i32` (path,len,info ptr,info size) |
| `aster_img_render` | `(i32,i32,i32,i32)i32` (path,len,args ptr,args size) |

`aster_img_render_args_t.dst_span` 被宿主按 `cap_w*cap_h*2` 上界强制校验，
应用越界写会 `ASTER_EINVAL`，不会踩宿主内存。示例见 `sdk/apps/gallery`。

### gfx **[2.2]** / **[3.1]**
**[2.2]** GFX 命令引擎（`gfx.h`）。只需 `requires=["display"]`；`aster_gfx_bmp_load`
的路径参数与 `aster_img_*` 同沙箱（需 `fs`，`/sd/...` 另需 `sd`）。命令只作用于
`ASTER_PIXEL_RGB565` surface；`cmds`/`arena` 整段先验范围，越界整体 `ASTER_EINVAL`。

| symbol | signature |
|---|---|
| `aster_gfx_exec` | `(i32,i32,i32,i32,i32,i32,i32,i32,i32)i32` (surface,cmds,n,arena,arena_len,px,py,pw,ph) |
| `aster_gfx_text_w` | `(i32,i32,i32)i32` (px,utf8,utf8_len) |
| **[3.1]** `aster_gfx_fonts_pending` | `()i32` — 0/1，「该重绘了吗」，见 §0 的 3.1 条目 |
| `aster_gfx_bmp_load` | `(i32,i32,i32,i32,i32,i32,i32)i32` (path,len,cap_w,cap_h,handle*,w*,h*) |
| `aster_gfx_bmp_free` | `(i32)i32` |

op 编码、脏区协议、文字基线约定与 AOT 注意事项见 [gfx-engine.md](gfx-engine.md)。

### kv
**[1.2]** 全部 `aster_kv_*` 需 `permissions=["kv"]`。

| symbol | signature |
|---|---|
| `aster_kv_set` | `(i32,i32,i32,i32)i32` |
| `aster_kv_get` | `(i32,i32,i32,i32)i32` -> val len |
| `aster_kv_delete` | `(i32,i32)i32` |

## 4. Event delivery

The Runtime owns one Zephyr thread per running app and an event queue.
`ASTER_EVENT_FRAME` is generated at the framerate; input/timer events are
enqueued. Each event is constructed inside the app's linear memory and
`aster_app_event(offset)` is invoked. Re-entrancy: lifecycle callbacks and event
callbacks never run concurrently.

**[1.1]** `ASTER_EVENT_KEY` 与 touch 在同一低延迟排空循环中投递（每 ~5ms 采样），
DOWN/UP 边沿保序。

## 5. Error handling & isolation

Any wasm trap / exception: Runtime logs `ASTER_TRAP <app>: <msg>`, calls
`aster_app_destroy` best-effort, releases every handle owned by the app
(surface/timer/file), frees linear memory, marks app state `CRASHED`.
The system (shell) remains usable.

## 6. Capability model

`aster_system_info_t.feature_flags` publishes the device capabilities; install
refuses apps whose manifest `requires` list is unsatisfied (warning only for
`optional`). API calls to absent features return `ASTER_ENOTSUP`.

**缺硬件时的规范（三层递进，系统不强杀）**：① 安装期 `requires` 不满足 →
拒装；② 安装期 `abi_min` 高于设备 → 拒装（`ASTER_EABI`）；③ 运行期调用未编译/
未使能的硬件 → `ASTER_ENOTSUP`，**App 应打日志/提示后自行 `aster_app_exit`**，
而不是等系统强退——强杀会丢应用自己的收尾与用户可见文案，且错误码已足够明确。
硬件开关本身由固件的 menuconfig 画像（`CONFIG_ASTER_*`）编译期决定，见 PORTING.md §2.2。

**[1.1]** `ASTER_FEATURE_KEYBOARD`（bit 3，随 `ASTER_HAS_KEYS` 画像上报）。

> **UI 形态不进 ABI**：分辨率、色深、触摸 vs 按键等差异，ABI 只如实上报
> **硬件能力**（`display_get_info` 的 width/height/native_format、
> `input_get_info().kinds`、`feature_flags`）。"这是彩屏触摸机还是单色按键机"
> 属于 **UI/应用层的形态判定**，由 launcher/工具库自行组合上述能力查询得出，
> 不在 `aster_system_info_t` 里塞 `form_factor` 之类的标签——那会把 UI 约定
> 固化进冻结的 ABI，且不同 UI 工具链的形态分类并不一致。硬件无关性由
> 能力查询保证，形态包（布局/字体/皮肤）归应用资源。

### 6.1 [1.3] Motion sensors (`sensor.h`)

Pull model, no event stream, no new permission — gated purely by capability.
SI units: **accel m/s²** (gravity included, a device flat on a table reads
≈ 9.8 on one axis), **gyro rad/s**.

```c
int32_t aster_accel_read(float *out_xyz, uint32_t size); /* size = 3*sizeof(float) */
int32_t aster_gyro_read(float *out_xyz, uint32_t size);
```

- `ASTER_EINVAL` if `size < 12` or the pointer is outside the app's memory;
- `ASTER_ENODEV` if the board lacks `ASTER_FEATURE_ACCELEROMETER`/`_GYROSCOPE`
  or the read fails at that moment — check `aster_system_has_feature()` first
  for a cheaper capability probe;
- the runtime coalesces reads to one I2C fetch per 50 ms so a tight app loop
  cannot hammer the bus. Apps poll from `aster_app_event(FRAME)` or a timer.

### 6.2 [1.3] Camera (`camera.h`)

Single-shot RGB565 capture (V1: one sensor, fixed size, pull model — no
continuous stream, mirroring how `hw cam` works but sandboxed to the caller).

```c
int32_t aster_cam_info(uint32_t *w, uint32_t *h);           /* native px, RGB565 */
int32_t aster_cam_read(uint8_t *buf, uint32_t size);         /* one frame, w*h*2 */
```

- `ASTER_ENODEV` when `ASTER_FEATURE_CAMERA` is absent; `ASTER_EINVAL` on a buffer
  smaller than one frame; `ASTER_ETIMEDOUT` if the sensor fails to deliver.
- `aster_cam_read` blocks the app worker for the capture (≤ ~1 s at QVGA on the
  reference port); V1 has no async frame event / multi-crop — deferred until a
  camera-first app needs it.

### 6.3 [1.7] / [1.8] / [1.9] / [1.13] Audio (`audio.h`) — 2.0 surface

Pull-model record + background playback (mirrors `hw audio play` / `hw audio
rec`, sandboxed to the caller). The native format is **per board** and is
asked for, never assumed: **[3.2]** `aster_audio_get_info` returns it (the
reference port answers **16 kHz, 16-bit, interleaved stereo s16le**, which is
what the 2.0-era `ASTER_AUDIO_*` macros hard-coded — they are gone, because a
macro in the SDK header is a board's number baked into an app that runs on
any board). **[1.7]** adds DAC volume and
a single background play session. **[1.8]** sessions also accept an MP3
bitstream; firmware decodes and resamples to native PCM. Record remains
PCM-only. **[1.9]** `aster_audio_start_file` streams a sandboxed path (typical
`/music/…`) — since 2.0 this is the only playback entry point (the in-memory
`play`/`start` copies are gone). **[1.13]** `seek(msec)` jumps the background
session (WAV sample-accurate, MP3 byte-proportional). `get_lyrics` copies
UTF-8 lyrics collected at start: ID3 USLT/SYLT first, else a sibling `.lrc`.
`get_state` v2 reports `msec_done` / `msec_total`.

```c
int32_t aster_audio_record(uint8_t *pcm, uint32_t size);       /* fills size bytes, blocks */

int32_t aster_audio_set_volume(int32_t percent);               /* 0..100, 0=mute, 100=0 dB */
int32_t aster_audio_get_volume(void);
int32_t aster_audio_start_file(const char *path, uint32_t path_len); /* sandbox path, stream */
int32_t aster_audio_pause(void);
int32_t aster_audio_resume(void);
int32_t aster_audio_stop(void);
int32_t aster_audio_get_state(aster_audio_state_t *st, uint32_t size);
int32_t aster_audio_get_info(aster_audio_info_t *info, uint32_t size); /* 3.2, per board */
int32_t aster_audio_seek(uint32_t msec);                       /* 1.13, ESTATE if idle */
int32_t aster_audio_get_lyrics(char *buf, uint32_t size);      /* 1.13, UTF-8, 0 if none */
```

- `ASTER_ENODEV` when the matching `ASTER_FEATURE_AUDIO_OUTPUT` / `_INPUT` bit is
  absent;
- `ASTER_EINVAL` if `size` is 0, the pointer is outside the app's memory, or PCM
  size is not a stereo-frame multiple; `set_volume` also rejects values
  outside 0..100;
- Volume percent is **loudness** (even dB steps from −40 dB at 1 % to 0 dB at
  100 %); 0 is mute. It is not a linear amplitude or raw DAC register scale;
- `ASTER_EBUSY` if `record` is called while a background session is running;
  `start_file` replaces the current session;
- `ASTER_ESTATE` if `pause`/`resume`/`seek` is called while idle; `stop` is OK when idle;
- `ASTER_EPERM` if `start_file` is called without manifest `fs`;
- `ASTER_ENOENT` if `start_file` path does not exist;
- `ASTER_EIO` if the codec or I2S path fails, or an MP3 payload cannot be decoded.
- There is still no mixer and no async completion event. `start_file` streams
  from the sandbox (SD-backed `/music/…` or app-private paths) with no
  whole-file copy, so the worker keeps running; pause/resume from a button.
  Blocking `record` is for short capture bursts on a frame callback.
  (`hw audio play|start /SD:/…` is the shell/debug equivalent outside the ABI.)

## 7. [1.2] Permissions & app manager

**Permissions are enforced.** The manifest `permissions` array (tokens:
`fs`, `kv`, `appmgr`, `sd`) is packed into a bitmask at install/scan time and
checked at the entry of every gated native — before sandbox path mapping,
handle decode, or any FS touch. Missing grant → `ASTER_EPERM` on the first
call. `sd` only unlocks the `/sd` → `/SD:` mapping; `aster_fs_*` still need `fs`.
Ungated families (log/time/system/display/surface/input/timer/sensor/camera/audio) need
no declaration. V1 recorded permissions without enforcing; 1.2 closes that
gap, so every app must declare what it actually calls.

### appmgr (permission `appmgr`)
| symbol | signature |
|---|---|
| `aster_appmgr_count` | `()i32` |
| `aster_appmgr_list` | `(i32,i32,i32)i32` (index, info_off, info_size) |
| `aster_appmgr_start` | `(i32,i32)i32` (id_off, id_len or -1) |
| `aster_appmgr_uninstall` | `(i32,i32)i32` |

`aster_appmgr_list` fills `aster_app_info_t`. Version **1** is through `flags`. Version **2** appends `category[16]` (`game` / `tool` / `system` / empty) and `has_icon`. Callers pass `info_size`; the runtime copies `min(info_size, sizeof)` so old wasm still works.

`aster_appmgr_start` is a **handoff**: the caller is cleanly torn down
(stop/destroy, resources reclaimed) and the target launches — the shape of
a single-app runtime. It returns only on refusal (`ASTER_EINVAL` self,
`ASTER_ENOENT` unknown, `ASTER_EBUSY` race). `uninstall` refuses self and any
running/paused app. The launcher is otherwise a normal sandboxed app: it
gets no hardware backdoor, only these four imports plus the ungated UI
surface — different screen skins are different launcher packages selected
by board config (`CONFIG_ASTER_SYSTEM_UI`), never a new ABI concept.

**Launch policy** (firmware, not ABI): if `CONFIG_ASTER_SYSTEM_UI` names an
installed app, it auto-starts at boot and is relaunched after any *other*
app exits ("return home", crash included). The UI's own exit is final —
no restart loop. Empty setting = headless (shell-only). Kconfig 抽象默认是
关；**参考 ESP32-S3 板 conf 打开桌面**（固件内置原生 launcher，`CONFIG_ASTER_LAUNCHER_NATIVE=y`；
guest-app 桌面 `aster.launcher.desktop` 已下沉进固件，仅留 `ASTER_SYSTEM_UI_CUSTOM` 作自定义皮肤回退）。
