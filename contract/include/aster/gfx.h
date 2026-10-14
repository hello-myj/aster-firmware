/* SPDX-License-Identifier: MIT
 * ASTER ABI 2.2 - the gfx command engine: an EasyX-style immediate-mode
 * painter for game/graphics apps. Apps encode cheap 24-byte draw commands
 * into a small ring (header-only helpers in sdk/lib/aster/aster_gfx.h) and
 * submit a whole frame with ONE native call; the firmware rasterizes
 * directly into the app's RGB565 surface buffer and pushes the union dirty
 * rect through the existing async display path.
 *
 * Why commands instead of pure app-side pixel loops: text needs the
 * firmware's native cpfont glyph caches, sprites need the image decoder,
 * and antialiasing/blending runs 1.5-3x faster in firmware C with 32-bit
 * word fills. Cheap primitives (set_pixel, tight rect fills) stay inline
 * in the app header -- both roads paint the SAME buffer, mix freely.
 *
 * Surfaces stay owned by aster_surface_*. Legacy drawing adds no extra
 * framebuffer; negotiated transactions use bounded private PSRAM staging.
 * Commands only apply to ASTER_PIXEL_RGB565 surfaces.
 */
#ifndef ASTER_GFX_ENGINE_ABI_H
#define ASTER_GFX_ENGINE_ABI_H

#include "types.h"

/* Fixed 24-byte command word (6 x u32). w[0].lo = op. Coordinates are
 * i16, sizes u16, colors RGB565 in a u16 field. Variable-length payloads
 * (UTF-8 text) live in a separate byte arena addressed by u32 offset. */
typedef struct {
	uint32_t w[6];
} aster_gfx_cmd_t;

#define ASTER_GFX_OP(c)   ((c)->w[0] & 0xFFu)
#define ASTER_GFX_P16(v)  ((int16_t)((v) & 0xFFFFu))	/* i16 in low half   */
#define ASTER_GFX_U16(v)  ((uint16_t)((v) & 0xFFFFu))	/* u16 in low half   */
#define ASTER_GFX_HI16(v) ((uint16_t)((v) >> 16))		/* u16 in high half  */
#define ASTER_GFX_PACK(lo, hi) \
	((uint32_t)(((uint32_t)(hi) << 16) | ((uint32_t)(lo) & 0xFFFFu)))

/* Field split. Most ops pack a PAIR of 16-bit values into one word with
 * ASTER_GFX_PACK(lo, hi); the accessors below are the only place that split is
 * written down, so the encoders (sdk/lib/aster/aster_gfx_engine.h) and the
 * firmware rasterizer (firmware/ui/ui_gfx.c) cannot drift apart. Word map
 * per op is in the enum below: coordinates are i16 pairs, sizes/radii and
 * colours are u16 halves, and a flag always rides in the HIGH half of the
 * field word it belongs to (bit 16 of the word):
 *
 *   ASTER_GFX_P16 / _P16H   signed   low / high half   x y x0 y0 cx cy
 *   ASTER_GFX_U16 / _HI16   unsigned low / high half   w h rx ry r, colors
 *   ASTER_GFX_HFLAG(v)      flag, bit 0 of v's high half
 *   ASTER_GFX_LFLAG(v)      flag, bit 0 of the whole word
 *
 * Whole-word fields carry no packing: TEXT arena_off (w3), BMP handle (w2).
 * The two masks are belt-and-braces clamps on values that steer a
 * destination rect / a coverage: the encoders document scale 1..4095 and
 * alpha 0..255, anything wider is truncated instead of honoured. */
#define ASTER_GFX_P16H(v)	((int16_t)((v) >> 16))		/* i16 in high half  */
#define ASTER_GFX_HFLAG(v) ((uint32_t)(((v) >> 16) & 1u))	/* flag at bit 16    */
#define ASTER_GFX_LFLAG(v) ((uint32_t)((v) & 1u))		/* flag at bit 0     */
#define ASTER_GFX_SCALE_MASK 0xFFFu			/* BMP_S/BMP_SA scale */
#define ASTER_GFX_ALPHA_MASK 0x1FFu			/* BMP_A/BMP_SA alpha */

typedef enum {
	ASTER_GFX_CLEAR   = 0,  /* w1=color                                        */
	ASTER_GFX_RECT_F  = 1,  /* w1=x|y<<16  w2=w|h<<16  w3=color                */
	ASTER_GFX_RECT_S  = 2,  /* same + w4=r|outline<<16 (ASTER_GFX_HFLAG)         */
	ASTER_GFX_LINE_AA = 3,  /* w1=x0|y0  w2=x1|y1  w3=color (Xiaolin Wu)       */
	ASTER_GFX_ELLIPSE = 4,  /* w1=cx|cy  w2=rx|ry  w3=color|stroke<<16 (HFLAG) */
	ASTER_GFX_TRI     = 5,  /* w1=p0 w2=p1 w3=p2 (x|y i16)  w4=color (flat)    */
	ASTER_GFX_GRAD    = 6,  /* w1=x|y w2=w|h w3=c0 w4=c1 w5=dir bit0 (0=v 1=h) */
	ASTER_GFX_TEXT    = 7,  /* w1=x|y(baseline) w2=px|fg<<16 w3=arena_off(u32)
			       *   w4=len(lo16) w5=bg|has_bg<<16 (ASTER_GFX_HFLAG) */
	/* 8 is RESERVED, deliberately left unnamed. It was declared here as
	 * ASTER_GFX_TEXT_BG ("TEXT too"), but no rasterizer dispatch case for
	 * it ever existed and no encoder ever emitted it: an app that used
	 * the name drew nothing and got no error back. The with-background
	 * text feature has always lived in TEXT's w5 above. Do not renumber
	 * or reuse 8 -- an old package compiled against a pre-3.0 SDK could
	 * still be sending it. */
	ASTER_GFX_BMP     = 9,  /* w1=x|y w2=handle(u32) w3 unused                 */
	ASTER_GFX_BMP_S   = 10, /*   + w4=scale&ASTER_GFX_SCALE_MASK (256 = 1:1)     */
	ASTER_GFX_BMP_A   = 11, /*   + w4=alpha&ASTER_GFX_ALPHA_MASK (0..255)        */
	ASTER_GFX_BMP_SA  = 12, /*   + w4=scale, w5=alpha (each masked above)      */
} aster_gfx_op_t;

/* TEXT: px selects the nearest native cpfont face (12..48), never a resized
 * glyph. Zero selects the generated UI default. SD resources must be present;
 * there is no runtime TTF fallback. y is the BASELINE. With has_bg, glyph
 * coverage blends against the supplied bg instead of the previous pixels;
 * clear a separate rectangle when replacing the entire advance/line area. */
/* BMP: handle from aster_gfx_bmp_load. Color key 0x0000 is transparent;
 * ALPHA multiplies coverage (255 = opaque). scale uses nearest sampling. */

/* Execute `n` commands against `surface`'s RGB565 buffer, then push the
 * union of the touched regions (or exactly the given rect when
 * present_w/present_h != 0) to the display. `cmds`/`arena` are linear
 * memory offsets the firmware validates wholesale before rasterizing --
 * a bad range fails atomically with nothing drawn. Returns the number of
 * commands executed, or a negative aster_error_t. */
ASTER_I("aster_gfx_exec") int32_t aster_gfx_exec(aster_handle_t surface,
					   const aster_gfx_cmd_t *cmds, uint32_t n,
					   const uint8_t *arena, uint32_t arena_len,
					   uint32_t present_x, uint32_t present_y,
					   uint32_t present_w, uint32_t present_h);

/* Negotiated frame protocol v1 over the EXISTING gfx_exec import. Old hosts
 * reject n=UINT32_MAX with EINVAL: query first, map that to ENOTSUP. No
 * reserved draw opcode or previously ignored flag bit is reused.
 * Control: cmds=NULL, n=CONTROL, arena=&control, arena_len=24,
 * present_x=MAGIC, present_y=1, present_w=present_h=0.
 * Query/begin use frame_id=0; begin returns a nonzero ID in the descriptor.
 * End/cancel must echo that ID. Flags/error are output only (input zero).
 * DRAW uses regular n with present_x=frame_id, present_y=MAGIC and w=h=0.
 * Stale/untagged batches cannot publish after cancel, pause or destroy.
 * While active, tagged gfx_exec batches draw privately; direct surface
 * present/flush is BUSY. Only successful END publishes, once, synchronously.
 * Canvas frames must START with CLEAR. No direct Guest pixel writes inside
 * the transaction. Failed/cancelled DRAW never modifies Guest or panel.
 * Physical I/O failure during END may partly update the LCD (no hardware
 * page flip); EIO is reported, never claimed to be an optical rollback.
 * v1 bounds: one active frame/APP, <=153600 pixels, 256 batches,
 * 8192 commands, 131072 arena bytes, 1000 ms from begin. */
#define ASTER_GFX_FRAME_CONTROL UINT32_MAX
#define ASTER_GFX_FRAME_MAGIC 0x47465231u
#define ASTER_GFX_FRAME_VERSION 1u
#define ASTER_GFX_FRAME_MAX_PIXELS 153600u
#define ASTER_GFX_FRAME_MAX_BATCHES 256u
#define ASTER_GFX_FRAME_MAX_COMMANDS 8192u
#define ASTER_GFX_FRAME_MAX_ARENA 131072u
#define ASTER_GFX_FRAME_TIMEOUT_MS 1000u
#define ASTER_GFX_FRAME_SUPPORTED 1u
#define ASTER_GFX_FRAME_CLIP_SUPPORTED 2u
#define ASTER_GFX_FRAME_FONT_EPOCH_SUPPORTED 4u
#define ASTER_GFX_FRAME_QUERY 0u
#define ASTER_GFX_FRAME_BEGIN 1u
#define ASTER_GFX_FRAME_END 2u
#define ASTER_GFX_FRAME_CANCEL 3u
#define ASTER_GFX_FRAME_SET_CLIP 4u
typedef struct {
 uint32_t size, version, action, frame_id, flags;
 int32_t error;
} aster_gfx_frame_control_t;

/* Optional, negotiated without new imports. QUERY advertises CLIP and FONT_EPOCH
 * flags; if FONT_EPOCH is present, QUERY.frame_id is a read-only bitmap epoch,
 * independent of repaint acknowledgement. Existing QUERY callers ignore it.
 * SET_CLIP uses this 32-byte descriptor, echoing the active frame ID. The
 * nonempty rectangle must fit the surface; it clips every subsequent private
 * DRAW until changed or frame end. BEGIN resets to the whole surface.
 * An uninitialized canvas must draw its full CLEAR before narrowing the clip.
 * Unsupported hosts advertise neither bit; clients must not silently downgrade.
 */
typedef struct {
 uint32_t size, version, action, frame_id, x, y, w, h;
} aster_gfx_frame_clip_t;


/* [3.1] "Should I repaint?" -- for event-driven apps that only redraw when
 * something happens.
 *
 * Baked-font bitmaps stream in from the SD card asynchronously, so a
 * codepoint that is not resident yet is rasterized as a correctly sized
 * BLANK and filled in later. An app that paints once per user action has no
 * reason to paint again, so that blank would stay on the glass until an
 * unrelated redraw. Call this once per ASTER_EVENT_FRAME and OR it into the
 * redraw condition:
 *
 *   if (dirty || aster_gfx_fonts_pending()) { render(); }
 *
 * It advances the fetch/commit pipeline and returns 1 when a bitmap landed
 * or a baked size became READY since the last call, 0 when the pipeline is
 * quiet. Continuous-render apps (games) repaint every frame anyway and can
 * ignore it. Never blocks; never returns an error. */
ASTER_I("aster_gfx_fonts_pending") int32_t aster_gfx_fonts_pending(void);

/* Text advance width of a UTF-8 run at `px` (same snap rules as ASTER_GFX_TEXT),
 * in pixels, WITHOUT drawing: pure resident metric-table lookup when the
 * baked size is READY. Negative on error. */
ASTER_I("aster_gfx_text_w") int32_t aster_gfx_text_w(uint32_t px, const char *utf8,
					       uint32_t utf8_len);

/* Load an image file (BMP/PNG/JPEG, fs-sandboxed path like aster_img_render)
 * into the firmware's sprite cache as packed RGB565, downscaled to fit
 * cap_w x cap_h. Returns ASTER_OK + *out_handle for ASTER_GFX_BMP*, or a
 * negative error. Handles own a bounded resource until free or APP exit.
 * Resource exhaustion returns an error; live handles are never evicted. */
ASTER_I("aster_gfx_bmp_load") int32_t aster_gfx_bmp_load(const char *path, uint32_t path_len,
						   uint32_t cap_w, uint32_t cap_h,
						   uint32_t *out_handle,
						   uint32_t *out_w, uint32_t *out_h);
ASTER_I("aster_gfx_bmp_free") int32_t aster_gfx_bmp_free(uint32_t handle);

#endif /* ASTER_GFX_ENGINE_ABI_H */
