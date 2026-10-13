# ASTER GFX 命令引擎（ABI 2.2 起；**3.0 起是 guest app 唯一的 UI 路径**）

面向游戏/图形 app 的**即时模式光栅引擎**。app 端把绘制意图编码成定长
24 字节命令压入小环形缓冲，一帧一次 native 调用提交；固件光栅器直接画进
surface 缓冲，并把脏区并集走既有异步刷屏流水线上屏。
设计目标：EasyX 式的编程手感 + 零拷贝渲染 + 文字/精灵复用固件已有的
cpfont/tiny_ttf 与 imgdec 基础设施。

缓冲有两种后端，由 `aster_surface_create` 的 `ASTER_SURFACE_F_PANEL_CANVAS`
选择，`aster_surface_buffer_t.flags` 回报实际拿到的是哪种（固件可以拒绝）：

- **普通 surface**：RGB565 缓冲在 app 线性内存里，`buf.ptr` 有效；命令环和
  内联像素循环画的是**同一块内存**，可以自由混用。present 时固件做一次字节序
  转换扫描（240×320 = 153 600 B，实测 ~7.9 ms/帧）。
- **面板画布**：`buf.ptr == NULL`，缓冲属于平台、已是屏要的字节序，只有引擎
  命令能画。省掉那次扫描，代价是**每帧必须整屏重画**——前后缓冲轮换，没画到的
  地方留着上上帧的像素。

> **面板画布在横屏构建里拿不到**（见 `verification.md` 63 号条目）：横屏下 app
> 坐标空间（240×320）不是逻辑屏（320×240），平台要把整幅画面缩放塞进玻璃，
> 而面板画布的前提正是"app 缓冲就是屏缓冲"。日志打
> `panel-canvas requested -> denied`，`buf.flags` 里没有那一位，app **必须**
> 按它读回来的标志分支（这本来就是契约）。代价实测 **+5.2 ms/帧**，全部落在
> flush 侧，`raster` 不变。竖屏构建不受影响。

无论哪种，命令都只对 `ASTER_PIXEL_RGB565` 生效。

**ABI 3.0 起没有第二套 UI**：`aster/ui.h` 控件运行时、`ui.bin`、`ASTER_EVENT_UI`
全部删除，app 界面一律在这条路径上自己画（见 [ui-runtime.md](ui-runtime.md) 墓碑页）。
阅读器 / 播放器界面后续同样基于本引擎重做。

规范头（单一事实源）：`abi/include/aster/gfx.h`；
app 侧编码器：`sdk/lib/aster/aster_gfx_engine.h`（命令环，header-only）+
`sdk/lib/aster/aster_gfx.h`（内联廉价图元，v1 路径）；
固件光栅器：`firmware/ui/ui_gfx.c` + `firmware/ui/pp_gfx.c`（pretty-poly 抗锯齿）；
native 入口：`firmware/runtime/aster_native.c`。
演示：`sdk/apps/gfxshow`；已移植到命令环：`sdk/apps/snake`、`sdk/apps/game.2048`、
`sdk/apps/reader`、`sdk/apps/player`（后两个是**事件驱动**的，靠
`aster_gfx_fonts_pending()` 才补得上异步落地的字形，见 §3）。
仍在 v1 内联路径上的：`sdk/apps/gallery`（它每帧整屏 present，v1 足够）。
文本测量/截断用 `sdk/lib/aster/aster_text.h`（`aster_text_w` / `aster_text_elide`，
与 `aster_gfx_text_w` 同一套 cpfont 度量，含 CJK 双宽）。

## 0. 抗锯齿几何（pretty-poly）

圆角矩形 / 椭圆 / 三角形 / 任意凸多边形由 **pretty-poly**（vendored 在
`firmware/modules/prettypoly/pretty-poly.h`，MIT）以**精确覆盖率**方式光栅化：
tile 内 4× 超采样，边缘像素得到 0..255 的真实覆盖率再与目标做 565 混合。
`pp_gfx.c` 是唯一定义 `PP_IMPLEMENTATION` 的 TU，负责把 pretty-poly 的
`malloc/calloc` 钩到一块 2 KB 静态 bump arena（每次 `ppg_path()` 结束整体复位，
不用 Zephyr 系统堆、不 realloc），并把 tile 回调桥接到目标 surface。

描边不再是「逐行端点连点」的手写算法——那样在曲率大的地方会断线或出现阶梯。
现在描边 = 外轮廓 + 沿顶点法线内缩 `stroke_w` 的内轮廓，靠 even-odd 填充自己
挖空，因此**与绕向无关**且行间不会撕裂。`ui_gfx.c` 里的旧整数扫描线实现保留为
arena 耗尽时的 fallback（`ppg_path` 返回非 0 时走），不会再产生之前的断线。
`RECT_S` 和 `ELLIPSE` 的仅描边模式使用向内 2 px 的描边；圆角与直边同宽，
避免 1 px 曲线经抗锯齿后视觉上过淡。

**退化轮廓必须先去重**（两条真实缺陷的根因，均已修）：

- 圆角矩形的相邻两段圆弧在 `2*r >= w|h` 时**共享同一个切点**，builder 会把该点
  输出两次 → 零长边没有方向，内缩法线的 `e1/l1` 除出来是噪声（`2e-6` 级的
  `cos/sin` 误差，精确 `==` 判不出来）→ 内缩点乱跑。现在 `gfx_shapes.h` 在
  emit 时按 `ASTER_GFX_EPS`(0.01 px) 去重（含首尾闭合那对），`ppg_path` 入口再用
  `compact_contour()` 兜一层，且 `r` 先按 `min(w,h)/2` 钳位。修复前
  `rr(144,60,60x44,r=22)` 描边返回 **-EINVAL（整帧什么都不画）**，这正是
  「长方形不正常」的真身。
- 太薄的外形（`w|h <= 2*stroke_w`）装不下洞：内缩轮廓会自交，even-odd 会挖掉
  随机楔形。现在直接**跳过挖洞**按实心画（比画出破洞或什么都不画都合理）。


## 1. 为什么是"命令环"而不是纯 app 侧画点

- **文字**需要固件里的 cpfont 常驻度量表 / tiny_ttf 字形缓存，app 拿不到；
- **精灵**需要 imgdec（BMP/PNG/JPEG 流式解码）与 PSRAM 精灵缓存；
- **AA/混合**在固件 C 里用 32-bit 字填充，比 wasm 内逐像素快 1.5–3×。

廉价的原语（set_pixel、紧凑矩形填充）仍留在 app 头文件里内联展开
（`aster_gfx.h` v1 路径）——两条路画的是**同一个缓冲**，可随意混用。
surface 生命周期仍归 `aster_surface_*`：本模块只加"画"，不加第二块帧缓冲。
命令只作用于 `ASTER_PIXEL_RGB565` 的 surface。

## 2. 命令编码（定长 6×u32）

`aster_gfx_cmd_t.w[0].lo = op`。坐标按 i16、尺寸按 u16 打包进同一 u32，
颜色 RGB565 占 u16 字段；变长载荷（UTF-8 文字）放独立字节 arena，
命令里只存 u32 偏移。

| op | 名称 | 字段 |
|---|---|---|
| 0 | `CLEAR` | w1=color |
| 1 | `RECT_F` | w1=x\|y<<16，w2=w\|h<<16，w3=color |
| 2 | `RECT_S` | 同上 + w4=r\|flags<<16（bit0=仅描边）。圆角矩形，逐行按角圆内缩 |
| 3 | `LINE_AA` | w1=x0\|y0，w2=x1\|y1，w3=color（Xiaolin Wu 抗锯齿） |
| 4 | `ELLIPSE` | w1=cx\|cy，w2=rx\|ry，w3=color\|flags<<16（bit0=仅描边） |
| 5 | `TRI` | w1=p0 w2=p1 w3=p2（x\|y i16），w4=color（平面着色，扫描线） |
| 6 | `GRAD` | w1=x\|y，w2=w\|h，w3=c0，w4=c1，w5=dir（0=竖 1=横），8.8 定点步进 |
| 7 | `TEXT` | w1=x\|baseline，w2=px\|fg<<16，w3=arena_off，w4=len，w5=bg\|has_bg<<16 |
| ~~8~~ | *保留* | **不要用**。见下 |
| 9–12 | `BMP/_S/_A/_SA` | w1=x\|y，w2=handle；_S 加 w4=scale（256=1:1）；_A 用 w4=alpha；_SA 各占一词 |

> **op 8 是保留号，故意不给名字。** 它在 ABI 2.2 曾以 `ASTER_GFX_TEXT_BG`
> （"TEXT too"）的形式声明过，但**光栅器从来没有对应的 dispatch 分支**，
> 编码器也从来没发过它——用了这个名字的 app 什么都画不出来，**而且拿不到错误码**
> （未知 op 走 `default: break`，`aster_gfx_exec` 照常返回命令条数）。
> 带背景的文字一直在 op 7 的 `w5` 里。3.0 把这个名字删掉了，号位保留不复用：
> 用旧 SDK 编过的包可能还在发它，复用号位会把"静默无操作"变成"画出别的东西"。
> 固件侧的耗时直方图第 8 格现在叫 `reserved`——那一格非零就说明有包在发保留 op。

文字约定：`y` 是**基线**（cpfont 度量约定）；`px` 在 ±4 px 内吸附到已烘焙
的 cpfont 字号（12..48），否则落 tiny_ttf 池；烘焙子集缺字按字形逐个回退。
`has_bg` 位放在 w5>>16，避免 bg=0xFFFF（合法白色）与"无背景"哨兵冲突。
精灵：颜色键 0x0000 透明；alpha 乘覆盖率（255 不透明）；scale 最近邻采样。

## 3. 五个 native 入口

```
int32_t aster_gfx_exec (surface, cmds, n, arena, arena_len,
                      present_x, present_y, present_w, present_h);
int32_t aster_gfx_text_w(uint32_t px, const char *utf8, uint32_t len);
int32_t aster_gfx_fonts_pending(void);                        /* [3.1] */
int32_t aster_gfx_bmp_load(path, path_len, cap_w, cap_h,
                         uint32_t *handle, *w, *h);
int32_t aster_gfx_bmp_free(uint32_t handle);
```

- `exec`：整段先按 surface 缓冲做范围校验，越界整体 `ASTER_EINVAL`、
  一画不画；随后逐命令光栅并累积脏盒。`present_w/h != 0` 时按显式矩形
  上屏，否则推脏区并集（自动裁剪到 surface 边界）。返回执行的命令数。
  入口处会泵一次异步字形管道（poll+commit）并**清掉重绘请求**——这一帧
  已经画进了流水线此刻能给的全部字形。
- `text_w`：只量宽不画。cpfont READY 时是纯常驻度量表查询，零 I/O。
- `fonts_pending`：**「该重绘了吗」**，返回 0/1。烤字位图走 SD 异步补入，
  未就位的码点先画成尺寸正确的空白框；只在事件驱动下重绘的 app（阅读器、
  播放器）如果不管它，空白框会一直留到下一次无关重绘。所以这类 app 把
  它并进重绘条件：

  ```c
  if (dirty || aster_gfx_fonts_pending()) { render(); }
  ```

  电平语义，不是边沿：读多少次结果一样，只有 `aster_gfx_exec` 真正出图才
  清零（`aster_font_sd_repaint_ack`），因此多读一次不会把信号吃掉——这正是
  它在 3.1 从边沿改成电平的原因：一个诊断 `aster_logf` 里顺手多读了一次，
  就足以让播放器永远收不到重绘信号、首帧的空白框一直停在屏上。调用本身
  会推进两级流水（不自行泵管道的 app 否则会把已取回的位图滞留在暂存区），
  不阻塞、无错误返回，流水线一静止立即变 0，所以可以直接拿它当重绘条件。
  持续重绘的 app（`gfxshow`、`snake` 这类每帧全画布的）不需要它。
- `bmp_load`：解码进固件 PSRAM 图片资源（6 个持有槽，单张 ≤160×160；耗尽时返回错误，须显式释放），
  句柄 1..6。槽满自动逐出最久未用；app 退出时 `aster_gfx_drop_all()`
  统一释放（runtime `ctx_cleanup` 钩子）。重载成本低，一般无需手动 free。

## 4. app 侧用法（`aster_ge_*`）

```c
#include <aster_gfx_engine.h>

static aster_ge_t ge;
static aster_gfx_cmd_t ring[160];
static uint8_t arena[1024];

/* init：aster_surface_create/acquire 之后 */
aster_ge_init(&ge, surf, W, H, ring, 160, arena, sizeof(arena));

/* 每帧（ASTER_EVENT_FRAME）：编码命令，最后一次性提交 */
aster_ge_clear(&ge, col);
aster_ge_grad(&ge, 0, 0, W, H, c0, c1, /*horizontal=*/1);
aster_ge_rect_r(&ge, x, y, w, h, col, 8, /*outline=*/0);
aster_ge_line_aa(&ge, x0, y0, x1, y1, col);
aster_ge_text(&ge, x, baseline, 16, fg, ASTER_GFX_NOBG, "混排 OK");
aster_ge_present(&ge);          /* = aster_gfx_exec + 复位环/arena */
```

- `aster_ge_room(g)` 正常环满会自动提交当前批次；无有效存储或已有错误时返回 NULL，不写入新命令；
- 文字通过联合预留命令槽与 arena 空间避免 payload 丢失；超长文字或提交失败会记录错误；`ASTER_GFX_NOBG=0xFFFF`（bg==NOBG 时 w5 清零）；
- 手写像素后调 `aster_ge_touch(g,x,y,w,h)` 并入 present 矩形；
- `aster_ge_present()` 返回并清除本帧首个错误，失败时丢弃尚未提交的命令；`aster_ge_error()` 只查询错误。已经自动提交的旧批次不能回滚。
- panel canvas 仍要求一次完整重绘；命令和文字预算必须避免中途提交，完整帧事务另行设计。

### 申请 panel canvas（每个应用自己选一次）

```c
aster_surface_desc_t d = { .size = sizeof(d), .version = 1,
			 .format = ASTER_PIXEL_RGB565,
			 .flags  = ASTER_SURFACE_F_PANEL_CANVAS };
aster_surface_create(&d, sizeof(d), &surf);
aster_surface_acquire(surf, &buf, sizeof(buf));
if (buf.flags & ASTER_SURFACE_F_PANEL_CANVAS) {
	/* buf.ptr == NULL：像素在固件帧缓冲里，只走引擎 */
}
```

拿不到就退回普通 surface，其它代码一行不用改（引擎在两条路上行为一致，
只是慢）。**用了 canvas 就必须每帧重画整块画布**，理由见 §6.1；引擎对
做不到的 app 会降级成同步 rect 发送并打一次警告，不会画错。

## 5. 固件侧管线要点（`ui_gfx.c`）

- 每个 op 画完即 `acc_merge` 并入脏盒；present 时钳到 surface 边界；
- 每次 `exec` 头部驱动 `aster_font_sd_poll()/commit()`：文字**白框异步
  协议**——cpfont 位图未驻留时当帧不画（度量正常、advance 正确），
  异步回填补上后下一帧自动显形。首帧无 SD 阻塞；
- `blend565` 通道展宽到 8-bit 做 lerp，结果回钳 5/6/5 位域；
- 精灵 blit 逐行整数步进采样（无除法进内循环），0x0000 颜色键跳过；
- **输出字节序由 `s_out_be` 一处决定**（见 §6.1）。所有 op 一律按 ABI
  RGB565 写，只有 `out_pack/out_unpack/out_blend` 三个 helper 知道怎么换序，
  所以换目标是「改一个参数」而不是「改三十处写点」。

## 6. 性能与真机数据（ESP32-S3，240×320，AOT）

> **口径**：本节所有读数都在**竖屏**构建上取得（那是参考板当时的出厂朝向）。
> 横屏构建的资源占用两者相同——朝向只交换宽高，**像素数是不变量**——但
> **面板画布那条快路径在横屏下不可用**，所以凡是以"拿得到面板画布"为前提的
> 数字，横屏都要按 flush 侧 +5.2 ms/帧 记账（63 号条目）。逐项差异见 §6.1。

### 6.1 panel canvas：把换序从「每帧一次拷贝」变成「写的时候顺手换掉」

初始实现是引擎光栅化进 app 的 LE surface，present 时再由平台整屏拷贝
并换序到 panel 帧缓冲。那一次拷贝实测 7.9 ms（153 600 B @ ~19 MB/s，
PSRAM→PSRAM 且 cache 冷），占掉整个 flush 的一半以上。

现在反过来：surface 用 `ASTER_SURFACE_F_PANEL_CANVAS` 申请，固件**不给**
app 侧像素内存（`acquire()` 返回 `ptr == NULL`），引擎直接把像素写进平台的
两块全屏帧缓冲之一，**并且写入时就是 panel 字节序**。于是：

- 换序的成本摊进商店指令，不再有独立的一遍 pass；
- present 只剩「刷 cache + 交给 DMA worker」，与下一帧光栅化并行。

代价是 app 必须**每帧重画整个画布**：两块帧缓冲轮换，正在画的那块里没有
上一帧的内容，只重画脏矩形会留下两帧前的残留。引擎对做不到的 app 有兜底
（改成同步 rect 发送 + 一次警告），但那条路的性能就是旧的。

| app | 是否用 canvas | 原因 |
|---|---|---|
| `gfxshow` / `snake` | 是 | 每帧 `aster_ge_clear()` 打底，天然满足全画布重绘 |
| 其它 app | 否 | 没有这个保证，继续用普通 LE surface（正确，只是慢） |

字节序正确性靠**调色板逐色比对**验证，不靠肉眼：截屏回读的帧与 app 源码里
的 `ASTER_RGB565(r,g,b)` 期望值必须逐字相等
（`gfxshow`: `0x1906`/`0x3693`/`0x0862`…，`snake`: `0x0863`/`0x15d0`/`0xf1eb`…）。

**但这条验证只覆盖引擎自己的缓冲区，覆盖不到 present 的换序**：截图优先读
`aster_gfx_last_frame()`，那是 app 侧/画布缓冲，字节序是引擎写进去的那一套；
上屏那一步错了它照样全绿。2026-09 就踩过：整屏 stage 拷贝里的 SWAR 写成了
`(v << 16) | (v >> 16)`（半个字交换 = 把相邻两个**像素**换了位置，而每个像素
自己的两个字节没动），于是**所有**走该路径的 app 玻璃上都是错的像素，而截图
验证全过。现在 present 路径的字节序由「玻璃记录」独立验证（见 §9），
换序只有两处实现：`sys_cpu_to_be16()`（标量）与 `display.c` 的 mask 版
`((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8)`（32 位双 lane），
两者必须逐字节一致，且与 `ui_gfx.c` 的 `out_pack()` 同约定。

### 6.2 优化后的每帧账

```
gfx tim: raster=21201 us flush=830 us sum=22031 us (max 27982/1649) cmds=15 flushed=76800 px
```

| 阶段 | 优化前 | 现在 | 说明 |
|---|---|---|---|
| raster（光栅化） | 27.0 ms | 21.2 ms | 见 §6.3 的逐 op 账 |
| flush（上屏） | 26.5 ms | **0.83 ms** | 只剩 `sys_cache_data_flush_range` + 投递；拷贝没了，SPI 在 worker 上与光栅并行 |
| 合计 | 54.2 ms → 18 fps | 22.0 ms → **41 fps** | 目标 37 fps，超额 |

**spi 时间没有消失，是藏到 raster 后面去了**：153 600 B @ 80 MHz 实测
18.9 ms 上线时间仍在，但现在与下一帧的光栅化重叠。所以新的地板是
`max(raster, spi)` = raster，**raster 降到 18.9 ms 以下就没收益了**。

帧率沿革（同一 app、同一面板）：18 → 24（多 slot 异步管道）→ 30
（32 位双子节换序拷贝）→ 38（panel canvas 去掉拷贝）→ 39（RECT_S 切角）
→ 41（GRAD 改行优先）。

### 6.3 光栅化的每 op 直方图（`CONFIG_ASTER_GFX_TIMING` 每 64 帧输出）

| op | µs/帧 | 占 raster | n/帧 | 相对优化前 |
|---|---|---|---|---|
| CLEAR（满屏 76800 px） | 5009 | 23% | 1 | 4022 → 5009 |
| **RECT_S**（圆角/描边） | **4714** | **22%** | 2 | **12885 → 4714** |
| ELLIPSE | 4138 | 19% | 2 | 2692–3385 → 4138 |
| GRAD | 2615 | 12% | 1 | 3457 → 2615 |
| TRI | 2178 | 10% | 1 | 1643 → 2178 |
| TEXT | 1977 | 9% | 2 | 1789 → 1977 |
| LINE_AA | 425 | 2% | 6 | 417 |

各项之和 21.1 ms ≈ `raster` 21.2 ms，账闭合。

**CLEAR 已经是 PSRAM 写的硬件地板**：153 600 B / 5.0 ms = **30.7 MB/s**，
与开机 benchmark 量到的 PSRAM 32 位写带宽 33 MB/s 吻合到 93%。
`fill_row` 已是 32 位双像素存储，这里没有余量，除非换存储介质。

CLEAR/ELLIPSE/TRI 比优化前**变贵**是字节序的代价：原来 CLEAR 写的是 app
wasm 线性内存（38 MB/s 那一段），现在写平台帧缓冲并带换序。总账仍然是
大赚——省下的 26 ms flush 换掉了几百微秒。

### 6.4 两个几何侧的优化

**RECT_S：把漂亮的路径只留在真的有弧度的地方。**
圆角矩形的边界只有四个 r×r 的角方块是曲线，其余全是轴对齐整数跨——
但原先整块 224×110 都送进 4 采样精确覆盖率光栅，绝大多数像素在做
「用矢量方法重新推导整数跨」的白工。

现在拆成：四个角方块各一次 `ppg_path`（`ppg_clip_t` 同时剪 x/y），
上下条带和中间条带直接 `fill_span`。为此给 vendored 的 `pretty-poly.h`
加了一处本地补丁（已与 `pretty-poly.h.orig` 分开保存）：`pp_render()`
的 tile 遍历本身也按 clip 剪枝，否则每次调用仍要空转走完整个包围盒的
tile（角落只占 1/8 的 tile，却付 100% 的遍历）。12.9 ms → 4.7 ms。

**GRAD：横向渐变改成行优先。**
横向渐变每列的数值在所有行上相同，但原实现是「外层列、内层行」，
于是每写一个像素就跨一个 pitch——160 行里每行单独落进一条 cache line,
对写回式 cache 是最坏形态。改成先把整行算好（顺带 `out_pack` 成目标
字节序）再逐行 `memcpy`，内层变成顺序 32 位存储，渐变算术也从每像素
一次降到每列一次。4168 → 2615 us。

### 6.5 留下的东西

- `raster` 21.2 ms 里 CLEAR 5.0 + 三块 AA 几何（RECT_S/ELLIPSE/TRI）11.0 ms。
  后者的单价是 pretty-poly 的 4 采样精确覆盖率（`PP_AA_X4` = 2×2），
  要再快只能降几何精度或换光栅化策略，那是画质取舍，不在引擎层。
- 想要 4×4 = 16 采样是 `PP_AA_X16`，成本约 4 倍。
- **命令编码仍然是免费的**（app 报 `enc=0–2 us`），每命令几次存储。
- 一个真机才暴露的缺陷已修：帧预算超支时 worker 原来**完全不让出 CPU**
  （`remain <= 0` 时跳过整个 sleep 循环），单核机器上把 shell 饿死，
  表现为 `aster app stop` 无响应、SD 也打不开。现在至少睡 1 ms（约一帧
  的 4%），换回可用性。

### 6.6 这套流畅度的内存代价（真被咬过一次）

面板帧只有 153 600 B，但 §6.1 的双缓冲 + 平台 staging 一共要**三块**：

| 用途 | 大小 | 谁用 |
|---|---|---|
| `swap_buf`（`CONFIG_ASTER_DISP_PSRAM_STAGE`） | 150 KB | 需要换序/旋转的普通 rect 收集（LVGL 带、非 canvas surface） |
| `s_front[0..1]`（`CONFIG_ASTER_DISP_FLIP_DBUF`） | 2 × 150 KB | flip 双缓冲：一块在被 DMA 送、一块在被画 |

**三块都来自 `SMH_REG_ATTR_EXTERNAL`——和 WAMR 池、8 档 cpfont 常驻表、
LVGL 池、相机、`.ast` 安装缓冲是同一个堆**（PSRAM 8 MB，`ESP_SPIRAM_HEAP_SIZE`
7.5 MB），没有分隔配额。加这两块 flip 前缓冲时正好把相机挤死：相机 3 个
150 KB 缓冲走的是 `CONFIG_VIDEO_BUFFER_USE_SHARED_MULTI_HEAP`（该模式下
`VIDEO_BUFFER_POOL_HEAP_SIZE` 不生效），要 450 KB，而当时堆里只剩
~300 KB 且最大空洞 ~256 KB → `cam_read` 一律 `-ENOMEM`。

排障工具是 `aster sys mem` 的两行探针（SMH 没有 stats API，只能借一块
立刻还一块来量）：

```
ext heap:   largest free block >= 1048576 B
ext heap:   8 x 150 KB blocks fit at once     <- 相机与双缓冲真正抢的就是这个
```

第二行是**同时持有**几块 150 KB，比「最大空洞」更能反映这类多缓冲需求：
修前是 `1`，相机必挂；腾出 1.5 MB 后是 `8`。改 `CONFIG_ASTER_WAMR_HEAP_SIZE`
（4 MB → 2.5 MB）就是为这件事：那 4 MB 是按已删的 LVGL 类 app（2.375 MB
线性内存）定的，在树 app 最大 heap peak 只有 296 KB。**以后再加全屏缓冲
前，先跑一遍 `aster sys mem` 看第二行。**

## 7. AOT 地雷与修复（必读）

wamrc 会把 wasm 里对 `memcpy`/`memset` 的调用按 **AOT builtin
（native 指针约定）** lower，并经 aot_loader 的重定位表**按名字**解析；
若宿主又在 env 表里注册了同名符号（本项目曾有 `n_memcpy`，
app-offset 约定），名字解析会先命中用户表 → 内置约定传来的 native 指针
被当成 app 偏移二次校验 → 拷贝静默丢失/崩溃。症状极具迷惑性：
**解释器运行完全正常，仅 AOT 首帧 trap**，且 trap 消息为空。

修复：`sdk/lib/aster/crt.c` 直接**定义** `memcpy/memmove/memset/memcmp`
（whole-archive 链进每个 app，`--gc-sections` 自动裁掉未用者），app 不再
import 这几个 env 符号。`strlen` 等字符串函数无内置冲突，维持导入。
验证数据集：导入 memcpy 的 gfxshow/snake 曾 AOT 必崩、修复后 RUNNING；
只导入 strlen 的 2048/clock 全程无恙；gallery（memset）修复后 RUNNING。
新 app 若需要大块内存操作，优先用 `aster_ge_*` 或直接写 surface。

## 8. 验证手段（主机侧 + 真机）

AA 形状的回归不看目视，靠两条可复现的通道（都不需要人眼）：

**A. 主机侧形状预览**（`firmware/ui/hostpreview/`，直接把固件源码链进 host）
```sh
cd aster/firmware/ui/hostpreview
cc -O2 -I.. -I../../modules/prettypoly -o preview preview.c ../pp_gfx.c -lm
./preview shapes.raw                      # 240x360 RGB565LE，与真机同一段光栅代码
python3 raw2png.py shapes.raw shapes.png 240 360
python3 verify_shapes.py shapes.raw 240 360
```
`verify_shapes.py` 检查的是**几何不变量**，不是像素快照：每个描边环必须是
8-连通单闭环（`deg<2` 计数为 0 → 「圆形线条丢失」的判据）、填充面积对解析
面积比 ≈1.0、圆角矩形四角确实被切掉、整幅有 ≥16 级覆盖率（AA 真的在羽化）。
`preview.c` 另打印轮廓顶点表（`dump_rr`）——`r == h/2` 时的重复切点是靠它
才定位到的。

**B. 真机几何探针**（`hostpreview/gfx_device_check.py`）
```sh
python3 sdk/tools/aster_deploy.py apps/gfxshow --no-build      # 已烧录则加 --no-flash
python3 firmware/ui/hostpreview/gfx_device_check.py shot.png
```
`shot.png` 由 `sdk/tools/shell_shot.py` 抓（`aster ui screenshot`，取**玻璃记录**：
`ui_screenshot.c` 先看 `aster_plat_display_glass_frame()`，它记的是交给线上的
画面，所以游戏 app 直推 surface 既不会截到过期 LVGL 屏，也骗不过换序缺陷）。
探针按 gfxshow 的固定场景解析：半径 30 的红环在
72 个角度上都必须有描边像素（±1.5 px 径向容差，环边缘的 AA 像素可能只有 25%
覆盖率，所以按**色相占优**判色而不是比对精确色值）、环内部不得有描边色
（洞被挖掉）、整环 8-连通无断头、圆角面板四角被切。三连拍全绿。

## 9. 快照调试限制（历史坑，已在 3.0 修掉一半）

- 老问题：`aster ui screenshot` 只走 `lv_snapshot_take(lv_screen_active())`，
  游戏 app 不拥有 LVGL 屏时截到过期内容。**已修**：gfx 引擎帧优先（见 §8 B）。
- **v1 内联路径的 app 也截不到**：`game.2048` / `gallery` 自己画 LE surface 后
  present，既不碰引擎也不碰 LVGL。引擎没帧、LVGL 又是「被 game app 挤掉的空白
  屏」，于是截出来是**一整片纯色**——看着像 app 渲染炸了，其实是工具在回答
  另一个问题。**已修**：固件记录「交给线上的画面」
  （`aster_plat_display_glass_frame()`，panel 字节序）。
- **玻璃记录曾经是「全有或全无」**：3.0 时它只是一根指向整屏帧的裸指针，
  任何局部矩形写都会 `glass_forget()` 把它清掉——于是 `gallery`（局部 band）
  和以后的 reader 这类 app 又回到上面那个纯色坑。**现在它是一块 150 KB 的
  PSRAM 影子缓冲**（`CONFIG_ASTER_SCREENSHOT` 才有）：整屏上屏仍然**零拷贝**
  地挂靠在那块 front buffer / bounce 上（47 fps 的热路径，每帧拷 150 KB 会多
  ~4.5 ms），一旦来了局部矩形就先把它拷进影子，此后每个矩形都合成进去，
  记录就再也没人能弄脏。代价是局部写多一次按行拷贝——全屏 present 的游戏
  不受影响，只有大面积动画的 band UI（LVGL 抽屉）会付。
- **截图优先级：玻璃帧 → 引擎帧 → LVGL**。玻璃帧是**唯一**在 present 路径
  **下游**的来源，也就是唯一能看见「换错序」的来源——§8 B 那次 SWAR 缺陷
  能一路发到玻璃上，正是因为当时的优先级读的是引擎帧（app 侧缓冲，换序
  **之前**的像素）。引擎帧现在只剩 headless（无屏无记录）兜底。
- **canvas surface 的帧是 panel 字节序**：`aster_gfx_last_frame()` 多返回一个
  `out_be` 标志，`ui_screenshot.c` 据此把整帧换回 ABI RGB565 再交给
  `shell_shot.py`（引擎帧那一支的换序集中在 `repack_abi()`）。玻璃帧一律
  panel 序，但**不再整帧拷一份**：`repack_abi()` 那块 150 KB 连续 PSRAM 分配
  正是过去 `SHOT err psram` 的来源，现在换序搭在分块发送的车上逐块做
  （`swap_rows`），CRC 也逐块累加，主机侧看到的字节流与从前完全一致。
- **装了旧包会骗人**：`aster_appsweep.py` 只负责跑，不负责重装。设备上残留的
  `aster.snake`/`aster.gfxshow` 是加 `ASTER_SURFACE_F_PANEL_CANVAS` 之前构建的，
  于是 canvas 全部被拒、每帧多付 7.4 ms 换序拷贝，而 `gfx tim` 里
  `flush=8158 us` 是唯一线索。改动 surface/引擎 ABI 后必须**重新
  `build.py` + 推包重装**，别拿旧包测新固件。固件现在在授予/拒绝 canvas 时
  打一行 `surface: ... panel-canvas requested -> granted|denied`。
- 仍在的限制：兜底那一支（LVGL PARTIAL，走 `lv_snapshot_take`）要一整块连续
  150 KB render buffer，app 跑久后池碎片化会 `SHOT err snapshot`。常规抓图已
  经不走它（玻璃记录覆盖 band UI 和 v1 内联 app），所以现在只在「无屏且引擎
  没帧」时才撞得到。排障优先信固件探针（`fps=`/`font prof:`）。
- 抓图时 shell 需要 CPU：43 节那个「帧预算超支不让出 CPU」的缺陷会让
  `aster ui screenshot` 一直拿不到调度（表现为脚本 120 s 超时、只抓到
  `SHOT` 之前的日志）。改帧率上限或修 pacing 后才能稳定抓图。
- `disp tim:` 有两种来源、格式曾经一样：staging gather 循环与
  `disp_direct_send()`（异步 worker）。后者 `gather` 恒为 0，曾被误读成
  「每帧多发了两次整屏」。现在后者带 `direct send (no gather)` 字样。


## 可协商完整帧协议 v1（本地开发中）

旧 `aster_gfx_exec` 导入及 draw opcode 保持不变；不复用保留 op 8。控制包使用 n=UINT32_MAX、cmds=NULL、arena 指向 24 字节且 4 字节对齐的 `aster_gfx_frame_control_t`、present_x=0x47465231、present_y=1、w=h=0。QUERY 返回支持标志；旧 Host 的 EINVAL 映射为不支持。BEGIN 返回 frame_id，END/CANCEL 必须回显。绘制批次以 present_x=frame_id、present_y=magic、w=h=0 标识，撤销后的旧批次不走旧的即时发布路径。

每 APP 单一活动事务，RGB565 私有 PSRAM 暂存，普通表面在 BEGIN 拷贝 Guest 像素，canvas 首批首条 CLEAR。私有栅格调用恢复截图的既有来源，未发布帧不会进入截图记录。表面直接 present/flush 会拒绝并令事务失败。错误锁存、批数/命令/arena/像素上限及绘制期限见公开 gfx.h；END 前输入错误或取消没有面板发送。状态为 FREE → DRAWING → QUEUED → IN_FLIGHT → FREE，首版 END 等待现有队列完成，缓存暂存待下帧复用。显示 slot 记录实际发送结果，checked drain 消费错误；旧 void drain 不改行为。

END 成功后更新 Guest 缓冲；失败批次、取消、暂停不更新 Guest 或发布帧。物理 I/O 故障可能已经改变 LCD 的部分内容，协议不保证光学回滚或垂直同步。底层发送/等待仍依赖现有驱动完成机制，不通过取消 DMA 等待提高速度。队列替换与异步流水线留待下一阶段。


## 有界脏区与开发板传输

栅格器按每条命令的真实裁剪结果收集区域，保留原并集输出供旧调用兼容。
内部集合由 `CONFIG_ASTER_GFX_DIRTY_REGIONS` 限定为 1～16 区，当前默认 8；
合并新增面积不超过 32 像素的区域，容量不足时合并代价最低的一对，不丢更新。
该阈值是保守初值，不是通用最优常量。并集覆盖全屏不表示实际损伤覆盖全屏，
只有一个完整区域或显式全屏请求才直接采用完整提交。

显式 present 区域保留原语义（也用于手绘 Guest 像素）；纯命令调用可以让 Host
推导区域，SDK 对应 `aster_ge_present_commands`。LCKFB 在帧末发送多个窗口，
微雪先逐区写入同一 AXS shadow，保持原起始/连续流协议，最后只发送一次整屏。
旋转沿既有坐标映射处理；canvas、scale-fit 和密度缩放保留完整路径。

普通事务跨批累积区域，成功显示完成后只写回这些 Guest 行。首次提交完整恢复，
之后仅在已发布表面与平台显示 generation 均匹配时使用保留损伤；其他写屏、
显示启停和方向变化均使 generation 改变。暂停/表面销毁撤销保留关系，END
失败也撤销关系（即使只改了 shadow、未发送），下一帧完整恢复。

`damage tim` 在统计启用时记录 64 次区域提交的区域数、实际成功窗口数、
已完成像素 payload 字节和耗时。未知的失败前缀不算成功字节，命令头不计入；
这些不是光学帧率。无新增整屏 DRAM 缓冲，没有恢复 PSRAM 直接 DMA。
双 band 的搬运/发送重叠仍需底层异步机制和峰值 DRAM 预算，尚未实施。
