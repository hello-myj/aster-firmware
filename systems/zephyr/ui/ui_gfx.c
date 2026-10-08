#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * ASTER gfx command engine (ABI 2.2) -- firmware rasterizer.
 *
 * Apps encode cheap 24-byte commands (sdk/lib/aster/aster_gfx_engine.h) and
 * submit a frame with ONE aster_gfx_exec() call; we rasterize straight into
 * the app's own RGB565 surface buffer (the same one the aster_gfx.h inline
 * helpers paint), then report the union dirty rect so the native shim can
 * push it through the existing async write_at pipeline.
 *
 * Threads: everything here runs on the app worker thread -- which IS the
 * UI thread in this firmware (WAMR and LVGL share it), so the cpfont
 * rings, its per-slot scratch and the sprite cache need no locking.
 *
 * Text goes through the public LVGL font chain: aster_ui_font() snaps to a
 * baked cpfont face whenever possible (per-glyph tiny_ttf fallback covers
 * unbaked codepoints). aster_font_sd_poll/commit at exec head advance the
 * async glyph-bitmap pipeline, so a cold glyph draws as a correct-size
 * blank for one frame and the real bitmap the next -- same behaviour the
 * LVGL labels have.
 *
 * Sprites: aster_gfx_bmp_load decodes (via imgdec, nearest) into one of six
 * PSRAM cache slots; color key 0x0000 = transparent, alpha multiplies
 * coverage. Integer per-channel RGB565 blend, no LUT (the multiply beats
 * the cache miss on this core).
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <math.h>

#include <aster/aster.h>

#include "ui_gfx.h"
#include "ui_gfx_internal.h" /* ppg_target_t, shared with pp_gfx.c */
#include "font_sd.h"
#include "ui_font.h"
#include "aster_mem.h"
#include "imgdec.h"
#include "pp_gfx.h"
#include "gfx_shapes.h"
#include "aster_color565.h" /* ch_up / blend565, shared with pp_gfx.c */
#include <lvgl.h>

LOG_MODULE_REGISTER(aster_gfx, LOG_LEVEL_INF);

/* ---------------- target + bounds accumulation ---------------- */

typedef struct {
	uint32_t x0, y0, x1, y1;
	bool has;
} acc_t;

static inline uint16_t *row_at(const ppg_target_t *t, uint32_t y)
{
	return t->px + (size_t)y * t->pitch;
}

/* ---------------- output byte order ----------------
 *
 * The rasterizer's internal colour convention is always plain ABI RGB565
 * (the value an app passes to aster_ge_rect etc. -- R in bits 15..11). Where
 * those words land is decided by s_out_be:
 *
 *   false -- the target is the app's own surface in wasm linear memory.
 *            Apps (and the screenshot path) read it, so it must hold ABI
 *            words verbatim.
 *   true  -- the target is a panel-order canvas: the glass latches RGB565
 *            MSB-first on the 8-bit DBI link, so the words must be byte
 *            swapped. Sending such a canvas is a straight hand-off with no
 *            per-frame conversion pass.
 *
 * Keeping the convention in one place means every op below stays written in
 * terms of ABI colours; only these four helpers know about the swizzle.
 *
 * One writer only: set_out_be() below. pp_gfx.c keeps its own copy of the
 * flag (it is linked standalone into the host preview), so the pair is
 * always flipped together -- nothing else may assign either one. */
static bool s_out_be;
/* Ordinary surfaces rasterize inside this box when it is set. A panel
 * canvas leaves it clear and repaints every command in full. */
static uint32_t s_clip_x, s_clip_y, s_clip_w, s_clip_h;

static inline void set_out_be(bool out_be)
{
	s_out_be = out_be;
	ppg_set_out_be(out_be);
}

static inline uint16_t out_pack(uint16_t le)
{
	return s_out_be ? sys_cpu_to_be16(le) : le;
}

static inline uint16_t out_unpack(uint16_t raw)
{
	return s_out_be ? sys_cpu_to_be16(raw) : raw;
}

/* blend an ABI colour over a raw target word, returning a raw target word */
static inline uint16_t out_blend(uint16_t raw_dst, uint16_t le_col, uint32_t a)
{
	return out_pack(blend565(out_unpack(raw_dst), le_col, a));
}

static inline void acc_merge(acc_t *a, uint32_t x0, uint32_t y0, uint32_t x1,
			     uint32_t y1)
{
	if (!a->has) {
		a->x0 = x0;
		a->y0 = y0;
		a->x1 = x1;
		a->y1 = y1;
		a->has = true;
		return;
	}
	if (x0 < a->x0) {
		a->x0 = x0;
	}
	if (y0 < a->y0) {
		a->y0 = y0;
	}
	if (x1 > a->x1) {
		a->x1 = x1;
	}
	if (y1 > a->y1) {
		a->y1 = y1;
	}
}

/* intersect with the surface; false = fully clipped */
static inline bool clip_rect(const ppg_target_t *t, int32_t x, int32_t y,
			     uint32_t w, uint32_t h, uint32_t *cx, uint32_t *cy,
			     uint32_t *cw, uint32_t *ch)
{
	if (w == 0u || h == 0u) {
		return false;
	}
	int32_t lim_x = (int32_t)t->w;
	int32_t lim_y = (int32_t)t->h;
	int32_t min_x = 0;
	int32_t min_y = 0;

	if (s_clip_w && s_clip_h) {
		min_x = (int32_t)s_clip_x;
		min_y = (int32_t)s_clip_y;
		if ((int32_t)(s_clip_x + s_clip_w) < lim_x) {
			lim_x = (int32_t)(s_clip_x + s_clip_w);
		}
		if ((int32_t)(s_clip_y + s_clip_h) < lim_y) {
			lim_y = (int32_t)(s_clip_y + s_clip_h);
		}
	}
	int32_t x0 = x > min_x ? x : min_x;
	int32_t y0 = y > min_y ? y : min_y;
	int32_t x1 = x + (int32_t)w < lim_x ? x + (int32_t)w : lim_x;
	int32_t y1 = y + (int32_t)h < lim_y ? y + (int32_t)h : lim_y;

	if (x1 <= x0 || y1 <= y0) {
		return false;
	}
	*cx = (uint32_t)x0;
	*cy = (uint32_t)y0;
	*cw = (uint32_t)(x1 - x0);
	*ch = (uint32_t)(y1 - y0);
	return true;
}

/* Every op that names a point packs it as w1 = x | y << 16 (i16 pair). */
static inline uint32_t cmd_xy0(const aster_gfx_cmd_t *c)
{
	return (uint32_t)ASTER_GFX_P16(c->w[1]);
}

static inline uint32_t cmd_xy1(const aster_gfx_cmd_t *c)
{
	return (uint32_t)ASTER_GFX_P16H(c->w[1]);
}

/* two-pixels-per-store splat. c arrives in ABI order and is swizzled here,
 * once per row instead of once per pixel. */
static void fill_row(uint16_t *dst, uint32_t n, uint16_t c)
{
	uint32_t i = 0;

	if (s_out_be) {
		c = sys_cpu_to_be16(c);
	}
	if ((uintptr_t)dst & 2u) {
		if (n == 0u) {
			return;
		}
		dst[0] = c;
		i = 1;
	}
	uint32_t cc = (uint32_t)c | ((uint32_t)c << 16);
	uint32_t *w = (uint32_t *)(dst + i);
	uint32_t nw = (n - i) / 2u;

	for (uint32_t k = 0; k < nw; k++) {
		w[k] = cc;
	}
	for (uint32_t k = 2u * nw + i; k < n; k++) {
		dst[k] = c;
	}
}

/* ---------------- ops (draw + merge their clipped bounds) ---------------- */

static void op_clear(ppg_target_t *t, const aster_gfx_cmd_t *c, acc_t *acc)
{
	uint16_t col = ASTER_GFX_U16(c->w[1]);
	uint32_t x0 = 0, y0 = 0, x1 = t->w, y1 = t->h;

	if (s_clip_w && s_clip_h) {
		x0 = s_clip_x;
		y0 = s_clip_y;
		x1 = s_clip_x + s_clip_w;
		y1 = s_clip_y + s_clip_h;
		if (x1 > t->w) {
			x1 = t->w;
		}
		if (y1 > t->h) {
			y1 = t->h;
		}
	}
	for (uint32_t y = y0; y < y1; y++) {
		fill_row(row_at(t, y) + x0, x1 - x0, col);
	}
	acc_merge(acc, x0, y0, x1, y1);
}

static void op_rect(ppg_target_t *t, const aster_gfx_cmd_t *c, acc_t *acc)
{
	uint32_t cx, cy, cw, ch;

	if (!clip_rect(t, (int32_t)cmd_xy0(c), (int32_t)cmd_xy1(c),
		       ASTER_GFX_U16(c->w[2]), ASTER_GFX_HI16(c->w[2]), &cx, &cy, &cw,
		       &ch)) {
		return;
	}
	uint16_t col = ASTER_GFX_U16(c->w[3]);

	for (uint32_t y = 0; y < ch; y++) {
		fill_row(row_at(t, cy + y) + cx, cw, col);
	}
	acc_merge(acc, cx, cy, cx + cw, cy + ch);
}

/* Paint an inclusive horizontal run [a, b] on row `py`, clipped to the
 * target, and merge it into the dirty accumulator. */
static void run_px(ppg_target_t *t, int32_t py, int32_t a, int32_t b,
		   uint16_t col, acc_t *acc)
{
	if (py < 0 || py >= (int32_t)t->h) {
		return;
	}
	if (a > b) {
		int32_t s = a;

		a = b;
		b = s;
	}
	if (a < 0) {
		a = 0;
	}
	if (b >= (int32_t)t->w) {
		b = (int32_t)t->w - 1;
	}
	if (s_clip_w && s_clip_h) {
		if (py < (int32_t)s_clip_y ||
		    py >= (int32_t)(s_clip_y + s_clip_h)) {
			return;
		}
		if (a < (int32_t)s_clip_x) {
			a = (int32_t)s_clip_x;
		}
		if (b >= (int32_t)(s_clip_x + s_clip_w)) {
			b = (int32_t)(s_clip_x + s_clip_w) - 1;
		}
	}
	if (b < a) {
		return;
	}
	fill_row(row_at(t, (uint32_t)py) + a, (uint32_t)(b - a + 1), col);
	acc_merge(acc, (uint32_t)a, (uint32_t)py, (uint32_t)b + 1u,
		  (uint32_t)py + 1u);
}

/* How far the left/right ends of row `yy` of a rounded rect pull inward,
 * i.e. r - (widest dx still inside the corner circle at that height). */
static uint32_t row_inset(uint32_t r, uint32_t h, uint32_t yy)
{
	uint32_t cy_in = 0;

	if (!r) {
		return 0;
	}
	if (yy < r) {
		cy_in = r - 1u - yy;
	} else if (yy + r >= h) {
		cy_in = yy - (h - r);
	}
	if (!cy_in) {
		return 0;
	}
	int32_t best = 0;

	for (int32_t dx = 0; dx <= (int32_t)r; dx++) {
		if (dx * dx + (int32_t)cy_in * (int32_t)cy_in <=
		    (int32_t)r * (int32_t)r) {
			best = dx;
		} else {
			break;
		}
	}
	return r - (uint32_t)best;
}

/* Rounded rect. Primary path: pretty-poly exact-coverage AA -- the four
 * corner circles are sampled into one convex closed polygon and filled
 * (or stroked as a 2 px even-odd ring for outline=1). If the renderer
 * refuses (arena pressure), fall back to the legacy integer scanline
 * stitcher below, which is crisp but stair-steps.
 * The contour sampling itself lives in gfx_shapes.h so the host-side shape
 * preview can render the exact same vertices the device does. */
static void op_rect_round_legacy(ppg_target_t *t, int32_t x, int32_t y, uint32_t w,
				 uint32_t h, uint32_t r, uint32_t outline,
				 uint16_t col, acc_t *acc)
{
	for (uint32_t yy = 0; yy < h; yy++) {
		int32_t py = y + (int32_t)yy;
		uint32_t inset = row_inset(r, h, yy);
		int32_t sx = x + (int32_t)inset;
		int32_t ex = x + (int32_t)w - (int32_t)inset - 1; /* inclusive */

		if (py < 0 || py >= (int32_t)t->h || ex < sx) {
			continue;
		}
		if (!outline) {
			run_px(t, py, sx, ex, col, acc);
			continue;
		}
		if (yy < 2u || yy + 2u >= h || w <= 4u || h <= 4u) {
			run_px(t, py, sx, ex, col, acc);
		} else {
			uint32_t inner_r = r > 2u ? r - 2u : 0u;
			uint32_t inner_inset = row_inset(inner_r, h - 4u, yy - 2u);
			int32_t inner_lo = x + 2 + (int32_t)inner_inset;
			int32_t inner_hi = x + (int32_t)w - 3 - (int32_t)inner_inset;

			run_px(t, py, sx, inner_lo - 1, col, acc);
			run_px(t, py, inner_hi + 1, ex, col, acc);
		}
	}
}

/* Straight axis-aligned span fill, clipped to the target. Used for the
 * parts of a rounded rect that need no feathering. */
static void fill_span(ppg_target_t *t, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
		      uint16_t col, acc_t *acc)
{
	int32_t min_x = 0, min_y = 0;
	int32_t max_x = (int32_t)t->w, max_y = (int32_t)t->h;

	if (s_clip_w && s_clip_h) {
		min_x = (int32_t)s_clip_x;
		min_y = (int32_t)s_clip_y;
		if ((int32_t)(s_clip_x + s_clip_w) < max_x) {
			max_x = (int32_t)(s_clip_x + s_clip_w);
		}
		if ((int32_t)(s_clip_y + s_clip_h) < max_y) {
			max_y = (int32_t)(s_clip_y + s_clip_h);
		}
	}
	int32_t cx0 = x0 < min_x ? min_x : x0;
	int32_t cy0 = y0 < min_y ? min_y : y0;
	int32_t cx1 = x1 > max_x ? max_x : x1;
	int32_t cy1 = y1 > max_y ? max_y : y1;

	if (cx0 >= cx1 || cy0 >= cy1) {
		return;
	}
	for (int32_t yy = cy0; yy < cy1; yy++) {
		fill_row(row_at(t, (uint32_t)yy) + cx0, (uint32_t)(cx1 - cx0), col);
	}
	acc_merge(acc, (uint32_t)cx0, (uint32_t)cy0, (uint32_t)cx1, (uint32_t)cy1);
}

/* Rounded rect, in one of two shapes:
 *
 *   fill    -- the full rounded body
 *   outline -- a 2 px ring just inside the outer contour
 *
 * Only the four corner squares carry a curved boundary; everything else is
 * axis-aligned and lands on exact integer spans. So the contour goes to
 * pretty-poly clipped to each corner square (four small renders, a couple
 * hundred pixels of supersampling total) and the rest is filled directly.
 * Sending the whole shape through the supersampled path instead costs
 * roughly ten times more for identical pixels.
 *
 * The middle strip is what keeps the two paths from touching the same
 * pixel: it spans [x+r, x+w-r) horizontally and [y+r, y+h-r) vertically,
 * which is exactly the region where the boundary is axis-aligned. */
static void op_rect_round(ppg_target_t *t, const aster_gfx_cmd_t *c, acc_t *acc)
{
	int32_t x = (int32_t)cmd_xy0(c), y = (int32_t)cmd_xy1(c);
	uint32_t w = ASTER_GFX_U16(c->w[2]), h = ASTER_GFX_HI16(c->w[2]);
	uint16_t col = ASTER_GFX_U16(c->w[3]);
	uint32_t r = ASTER_GFX_U16(c->w[4]);
	uint32_t outline = ASTER_GFX_HFLAG(c->w[4]);
	int32_t R, x1, y1;

	if (!w || !h) {
		return;
	}
	if (r > w / 2u) {
		r = w / 2u;
	}
	if (r > h / 2u) {
		r = h / 2u;
	}
	acc_merge(acc, (uint32_t)(x < 0 ? 0 : x), (uint32_t)(y < 0 ? 0 : y),
		  (uint32_t)(x + (int32_t)w), (uint32_t)(y + (int32_t)h));

	/* r<2 or a degenerate box: no curvature to feather, legacy is exact */
	if (r < 2u || w < 2u || h < 2u) {
		op_rect_round_legacy(t, x, y, w, h, r, outline, col, acc);
		return;
	}

	R = (int32_t)r;
	x1 = x + (int32_t)w;
	y1 = y + (int32_t)h;

	/* The four corner squares. Each holds exactly one quarter-arc of the
	 * outer contour, and (for the ring) the matching piece of the inset
	 * contour, so even-odd fill resolves the corner of the ring. */
	{
		float xy[ASTER_GFX_SHAPE_MAXV * 2u];
		uint32_t n = aster_gfx_rr_polygon(xy, x, y, w, h, r);

		if (n < 3u) {
			op_rect_round_legacy(t, x, y, w, h, r, outline, col, acc);
			return;
		}
		const struct {
			int32_t x0, y0, x1, y1;
		} corners[4] = {
			{ x, y, x + R, y + R },              /* top-left */
			{ x1 - R, y, x1, y + R },            /* top-right */
			{ x, y1 - R, x + R, y1 },            /* bottom-left */
			{ x1 - R, y1 - R, x1, y1 },          /* bottom-right */
		};

		for (int i = 0; i < 4; i++) {
			ppg_clip_t cl = { corners[i].x0, corners[i].y0,
					  corners[i].x1, corners[i].y1 };
			int32_t bx0, by0, bx1, by1;

			if (ppg_path(t, xy, (int)n,
				     outline ? 2.0f : 0.0f, col, &cl, &bx0, &by0,
				     &bx1, &by1) != 0) {
				/* Out of arena or bad contour: fall back for
				 * the whole shape rather than leave it half
				 * drawn. Earlier corners may be painted
				 * twice, which is idempotent for opaque
				 * colours and harmless for the acc. */
				op_rect_round_legacy(t, x, y, w, h, r, outline,
						     col, acc);
				return;
			}
			acc_merge(acc, (uint32_t)bx0, (uint32_t)by0, (uint32_t)bx1,
				  (uint32_t)by1);
		}
	}

	if (!outline) {
		/* Body: three strips. The middle one is the full width; the two
		 * above and below it stop short of the corners. */
		fill_span(t, x + R, y, x1 - R, y + R, col, acc);
		fill_span(t, x, y + R, x1, y1 - R, col, acc);
		fill_span(t, x + R, y1 - R, x1 - R, y1, col, acc);
		return;
	}

	/* Straight spans use the same 2 px inset as the curved corners. */
	fill_span(t, x + R, y, x1 - R, y + 2, col, acc);
	fill_span(t, x + R, y1 - 2, x1 - R, y1, col, acc);
	fill_span(t, x, y + R, x + 2, y1 - R, col, acc);
	fill_span(t, x1 - 2, y + R, x1, y1 - R, col, acc);
}

static void op_ellipse(ppg_target_t *t, const aster_gfx_cmd_t *c, acc_t *acc)
{
	int32_t ccx = (int32_t)cmd_xy0(c), ccy = (int32_t)cmd_xy1(c);
	uint32_t rx = ASTER_GFX_U16(c->w[2]), ry = ASTER_GFX_HI16(c->w[2]);
	uint16_t col = ASTER_GFX_U16(c->w[3]);
	uint32_t stroke = ASTER_GFX_HFLAG(c->w[3]);

	if (!rx || !ry) {
		return;
	}
	int32_t bx0 = ccx - (int32_t)rx, bx1 = ccx + (int32_t)rx + 1;
	int32_t by0 = ccy - (int32_t)ry, by1 = ccy + (int32_t)ry + 1;

	acc_merge(acc, bx0 < 0 ? 0 : (uint32_t)bx0, by0 < 0 ? 0 : (uint32_t)by0,
		  bx1 > (int32_t)t->w ? t->w : (uint32_t)bx1,
		  by1 > (int32_t)t->h ? t->h : (uint32_t)by1);

	static float xy[64 * 2u]; /* 60 max verts, keep off the 2K stack */
	uint32_t n = aster_gfx_ell_polygon(xy, ccx, ccy, rx, ry);
	int32_t ix0, iy0, ix1, iy1;

	if (ppg_path(t, xy, (int)n,
		     stroke ? 2.0f : 0.0f, col, NULL, &ix0, &iy0, &ix1,
		     &iy1) == 0) {
		return;
	}

	/* legacy fallback: per-row integer span, outline stitched */
	int32_t prev_lo = 0, prev_hi = 0;
	bool have_prev = false;

	for (uint32_t yy = 0; yy < 2u * ry + 1u; yy++) {
		int32_t py = by0 + (int32_t)yy;
		int64_t dy = (int64_t)yy - (int64_t)ry;
		int32_t hw = -1;

		if (py < 0 || py >= (int32_t)t->h) {
			continue;
		}
		for (int32_t dx = 0; dx <= (int32_t)rx; dx++) {
			if ((int64_t)dx * dx * (int64_t)ry * ry +
			    dy * dy * (int64_t)rx * rx <=
			    (int64_t)rx * rx * (int64_t)ry * ry) {
				hw = dx;
			} else {
				break;
			}
		}
		if (hw < 0) {
			continue;
		}
		int32_t px0 = ccx - hw, px1 = ccx + hw; /* inclusive */

		if (!stroke) {
			run_px(t, py, px0, px1, col, acc);
			continue;
		}
		if (have_prev) {
			run_px(t, py, px0, prev_lo, col, acc);
			run_px(t, py, px1, prev_hi, col, acc);
		} else {
			run_px(t, py, px0, px1, col, acc);
		}
		prev_lo = px0;
		prev_hi = px1;
		have_prev = true;
	}
}

/* ---------------- antialiased line (Xiaolin Wu) ---------------- */

static inline void px_a(ppg_target_t *t, int32_t x, int32_t y, uint16_t c,
			uint32_t a)
{
	if (x < 0 || y < 0 || x >= (int32_t)t->w || y >= (int32_t)t->h ||
	    a == 0u) {
		return;
	}
	if (s_clip_w && s_clip_h &&
	    (x < (int32_t)s_clip_x || y < (int32_t)s_clip_y ||
	     x >= (int32_t)(s_clip_x + s_clip_w) ||
	     y >= (int32_t)(s_clip_y + s_clip_h))) {
		return;
	}
	uint16_t *d = row_at(t, (uint32_t)y) + x;

	/* blend565() speaks ABI order; out_blend() converts both ways, so the
	 * expensive swizzle rides along with the blend instead of adding a
	 * separate pass. */
	*d = out_blend(*d, c, a);
}

/* general case: steep handled by axis swap at the caller */
static void wu_x(ppg_target_t *t, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
		 uint16_t col, bool steep, acc_t *acc)
{
	if (x0 > x1) {
		int32_t s = x0;

		x0 = x1;
		x1 = s;
		s = y0;
		y0 = y1;
		y1 = s;
	}
	float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
	float grad = dx != 0.0f ? dy / dx : 1.0f;

	/* first endpoint */
	{
		float xend = (float)x0 + 0.5f;
		float yend = (float)y0 + 0.5f * grad;
		int32_t ix = (int32_t)xend, iy = (int32_t)yend;
		uint32_t frac = (uint32_t)((yend - (float)iy) * 255.0f);

		if (frac > 255u) {
			frac = 255u;
		}
		if (steep) {
			px_a(t, iy, ix, col, 255u - frac);
			px_a(t, iy + 1, ix, col, frac);
			acc_merge(acc, (uint32_t)(ix < 0 ? 0 : ix),
				  (uint32_t)(iy < 0 ? 0 : iy),
				  (uint32_t)(ix + 1), (uint32_t)(iy + 2));
		} else {
			px_a(t, ix, iy, col, 255u - frac);
			px_a(t, ix, iy + 1, col, frac);
			acc_merge(acc, (uint32_t)(ix < 0 ? 0 : ix),
				  (uint32_t)(iy < 0 ? 0 : iy),
				  (uint32_t)(ix + 1), (uint32_t)(iy + 2));
		}
	}
	/* last endpoint */
	{
		float xend = (float)x1 + 0.5f;
		float yend = (float)y1 + 0.5f * grad;
		int32_t ix = (int32_t)xend, iy = (int32_t)yend;
		uint32_t frac = (uint32_t)((yend - (float)iy) * 255.0f);

		if (frac > 255u) {
			frac = 255u;
		}
		if (steep) {
			px_a(t, iy, ix, col, 255u - frac);
			px_a(t, iy + 1, ix, col, frac);
		} else {
			px_a(t, ix, iy, col, 255u - frac);
			px_a(t, ix, iy + 1, col, frac);
		}
		acc_merge(acc, (uint32_t)(ix < 0 ? 0 : ix),
			  (uint32_t)(iy < 0 ? 0 : iy),
			  (uint32_t)(ix + 1), (uint32_t)(iy + 2));
	}
	/* main body */
	float intery = (float)y0 + 0.5f * grad + grad;
	int32_t yint = (int32_t)intery;
	uint32_t f8 = (uint32_t)((intery - (float)yint) * 255.0f);

	for (int32_t x = x0 + 1; x < x1; x++) {
		yint = (int32_t)intery;
		f8 = (uint32_t)((intery - (float)yint) * 255.0f);
		if (f8 > 255u) {
			f8 = 255u;
		}
		if (steep) {
			px_a(t, yint, x, col, 255u - f8);
			px_a(t, yint + 1, x, col, f8);
		} else {
			px_a(t, x, yint, col, 255u - f8);
			px_a(t, x, yint + 1, col, f8);
		}
		intery += grad;
	}
}

static void op_line(ppg_target_t *t, const aster_gfx_cmd_t *c, acc_t *acc)
{
	int32_t x0 = (int32_t)cmd_xy0(c), y0 = (int32_t)cmd_xy1(c);
	int32_t x1 = (int32_t)ASTER_GFX_P16(c->w[2]);
	int32_t y1 = (int32_t)ASTER_GFX_P16H(c->w[2]);
	uint16_t col = ASTER_GFX_U16(c->w[3]);

	int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
	int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
	int32_t mx = x0 < x1 ? x0 : x1, my = y0 < y1 ? y0 : y1;
	int32_t Mx = x0 > x1 ? x0 : x1, My = y0 > y1 ? y0 : y1;

	acc_merge(acc, mx < 0 ? 0 : (uint32_t)mx, my < 0 ? 0 : (uint32_t)my,
		  (uint32_t)Mx + 1u, (uint32_t)My + 1u);
	if (dx >= dy) {
		wu_x(t, x0, y0, x1, y1, col, false, acc);
	} else {
		wu_x(t, y0, x0, y1, x1, col, true, acc);
	}
}

static void op_tri(ppg_target_t *t, const aster_gfx_cmd_t *c, acc_t *acc)
{
	int32_t x0 = (int32_t)cmd_xy0(c), y0 = (int32_t)cmd_xy1(c);
	int32_t x1 = (int32_t)ASTER_GFX_P16(c->w[2]);
	int32_t y1 = (int32_t)ASTER_GFX_P16H(c->w[2]);
	int32_t x2 = (int32_t)ASTER_GFX_P16(c->w[3]);
	int32_t y2 = (int32_t)ASTER_GFX_P16H(c->w[3]);
	uint16_t col = ASTER_GFX_U16(c->w[4]);

	int32_t minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
	int32_t maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
	int32_t miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
	int32_t maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);

	if (minx < 0) {
		minx = 0;
	}
	if (miny < 0) {
		miny = 0;
	}
	if (maxx > (int32_t)t->w - 1) {
		maxx = (int32_t)t->w - 1;
	}
	if (maxy > (int32_t)t->h - 1) {
		maxy = (int32_t)t->h - 1;
	}
	if (maxx < minx || maxy < miny) {
		return;
	}
	int32_t area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);

	if (area == 0) {
		return;
	}

	acc_merge(acc, (uint32_t)minx, (uint32_t)miny, (uint32_t)maxx + 1u,
		  (uint32_t)maxy + 1u);

	/* pretty-poly AA triangle; the even-odd span builder handles any
	 * winding, so pos/neg area both work. */
	const float xy[6] = { (float)x0, (float)y0, (float)x1, (float)y1,
			      (float)x2, (float)y2 };
	int32_t bx0, by0, bx1, by1;

	if (ppg_path(t, xy, 3, 0.0f, col, NULL,
		     &bx0, &by0, &bx1, &by1) == 0) {
		return;
	}

	/* legacy fallback: integer edge-function scanline */
	bool pos = area > 0;

	for (int32_t py = miny; py <= maxy; py++) {
		int32_t run = -1;

		for (int32_t px = minx; px <= maxx + 1; px++) {
			bool in = false;

			if (px <= maxx) {
				int32_t e0 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
				int32_t e1 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
				int32_t e2 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2);

				in = pos ? (e0 >= 0 && e1 >= 0 && e2 >= 0)
					 : (e0 <= 0 && e1 <= 0 && e2 <= 0);
			}
			if (in && run < 0) {
				run = px;
			} else if (!in && run >= 0) {
				fill_row(row_at(t, (uint32_t)py) + run,
					 (uint32_t)(px - run), col);
				run = -1;
			}
		}
	}
}

/* 565 from the 8-bit fixed-point gradient accumulators (x256) */
static inline uint16_t grad_px(int32_t rr, int32_t rg, int32_t rb)
{
	uint32_t R = (uint32_t)(rr + 128) >> 8, G = (uint32_t)(rg + 128) >> 8,
		 B = (uint32_t)(rb + 128) >> 8;

	R = R > 255u ? 255u : R;
	G = G > 255u ? 255u : G;
	B = B > 255u ? 255u : B;
	return (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
}

/* one packed gradient row, for the horizontal case in op_grad: a static
 * buffer rather than a local because the renderer is single-threaded and
 * the app worker stack is not worth 1 KB of (see s_last_buf et al). */
#define GFX_GRAD_ROW_MAX 512u
static uint16_t s_grad_row[GFX_GRAD_ROW_MAX];

static void op_grad(ppg_target_t *t, const aster_gfx_cmd_t *c, acc_t *acc)
{
	int32_t x = (int32_t)cmd_xy0(c), y = (int32_t)cmd_xy1(c);
	uint32_t w = ASTER_GFX_U16(c->w[2]), h = ASTER_GFX_HI16(c->w[2]);
	uint16_t c0 = ASTER_GFX_U16(c->w[3]), c1 = ASTER_GFX_U16(c->w[4]);
	uint32_t horiz = ASTER_GFX_LFLAG(c->w[5]);
	uint32_t cx, cy, cw, ch;

	if (!clip_rect(t, x, y, w, h, &cx, &cy, &cw, &ch)) {
		return;
	}
	uint32_t span = horiz ? cw : ch;

	if (span == 0u) {
		return;
	}
	int32_t r0 = ch_up((c0 >> 11) & 0x1F, 5) * 256;
	int32_t g0 = ch_up((c0 >> 5) & 0x3F, 6) * 256;
	int32_t b0 = ch_up(c0 & 0x1F, 5) * 256;
	int32_t r1 = ch_up((c1 >> 11) & 0x1F, 5) * 256;
	int32_t g1 = ch_up((c1 >> 5) & 0x3F, 6) * 256;
	int32_t b1 = ch_up(c1 & 0x1F, 5) * 256;
	int32_t dr = (r1 - r0) / (int32_t)span;
	int32_t dg = (g1 - g0) / (int32_t)span;
	int32_t db = (b1 - b0) / (int32_t)span;
	int32_t rr = r0, rg = g0, rb = b0;

	/* A horizontal gradient puts the SAME value down every row of the
	 * rect, so the obvious "walk a column at a time" loop strides by
	 * pitch and lands a lone 2-byte write in a fresh cache line, 160+
	 * rows per column, against a write-back target. Build the row once
	 * (already in the target's byte order) and blit it instead: the
	 * inner copy becomes sequential 32-bit stores, and we pay the
	 * gradient arithmetic once per column rather than once per pixel. */
	if (horiz && cw <= GFX_GRAD_ROW_MAX) {
		for (uint32_t i = 0; i < cw; i++) {
			s_grad_row[i] = out_pack(grad_px(rr, rg, rb));
			rr += dr;
			rg += dg;
			rb += db;
		}
		for (uint32_t yy = 0; yy < ch; yy++) {
			memcpy(row_at(t, cy + yy) + cx, s_grad_row,
			       (size_t)cw * 2u);
		}
		acc_merge(acc, cx, cy, cx + cw, cy + ch);
		return;
	}

	for (uint32_t i = 0; i < span; i++) {
		uint16_t col = grad_px(rr, rg, rb);

		if (horiz) {
			uint16_t pc = out_pack(col);

			for (uint32_t yy = 0; yy < ch; yy++) {
				row_at(t, cy + yy)[cx + i] = pc;
			}
		} else {
			fill_row(row_at(t, cy + i) + cx, cw, col);
		}
		rr += dr;
		rg += dg;
		rb += db;
	}
	acc_merge(acc, cx, cy, cx + cw, cy + ch);
}

/* ---------------- text ---------------- */

static uint32_t utf8_next_t(const uint8_t **p)
{
	const uint8_t *s = *p;
	uint8_t ch = s[0];
	uint32_t cp, extra, i;

	if (ch == 0u) {
		return 0;
	}
	if (ch < 0x80u) {
		*p = s + 1;
		return ch;
	}
	if ((ch & 0xE0u) == 0xC0u) {
		cp = ch & 0x1Fu;
		extra = 1;
	} else if ((ch & 0xF0u) == 0xE0u) {
		cp = ch & 0xFu;
		extra = 2;
	} else if ((ch & 0xF8u) == 0xF0u) {
		cp = ch & 7u;
		extra = 3;
	} else {
		*p = s + 1;
		return 0xFFFD;
	}
	for (i = 0; i < extra; i++) {
		if ((s[1 + i] & 0xC0u) != 0x80u) {
			*p = s + 1;
			return 0xFFFD;
		}
		cp = (cp << 6) | (s[1 + i] & 0x3Fu);
	}
	*p = s + 1 + extra;
	return cp;
}

int32_t aster_gfx_text_measure(uint8_t px, const char *utf8, uint32_t len)
{
	const lv_font_t *f = aster_ui_font(px);
	const uint8_t *p = (const uint8_t *)utf8, *end = p + len;
	int32_t adv = 0;
	int32_t maxrun = 0;

	while (p < end) {
		uint32_t cp = utf8_next_t(&p);

		if (cp == 0u) {
			break;
		}
		if (cp == '\n') {
			if (adv > maxrun) {
				maxrun = adv;
			}
			adv = 0;
			continue;
		}
		adv += (int32_t)lv_font_get_glyph_width(f, cp, 0);
	}
	return adv > maxrun ? adv : maxrun;
}

static void op_text(ppg_target_t *t, const aster_gfx_cmd_t *c, const uint8_t *arena,
		    uint32_t arena_len, acc_t *acc)
{
	int32_t x = (int32_t)cmd_xy0(c), base = (int32_t)cmd_xy1(c);
	uint32_t px = ASTER_GFX_U16(c->w[2]);
	uint16_t fg = ASTER_GFX_HI16(c->w[2]);
	uint32_t off = c->w[3]; /* arena offset: a whole word, not a packed half */
	uint32_t len = ASTER_GFX_U16(c->w[4]);
	uint32_t bgw = c->w[5];
	bool has_bg = ASTER_GFX_HFLAG(bgw);
	uint16_t bg = ASTER_GFX_U16(bgw);

	/* off is the app's raw w[3]: check it against the arena before forming
	 * off + len, which wraps in u32 and would let a huge offset through. */
	if (len == 0u || off > arena_len || len > arena_len - off) {
		return;
	}
	const uint8_t *p = arena + off, *end = p + len;

	/* px 0 = "whatever the UI uses"; aster_ui_font() owns that rule now
	 * (and the ASTER_FONT_UI_PX macro behind it), so measuring and drawing
	 * cannot disagree about what px 0 means. `fpx` is only the
	 * missing-glyph advance, which is a guess by nature. */
	const lv_font_t *f = aster_ui_font((uint8_t)px);
	uint8_t fpx = (uint8_t)(px ? px : ASTER_FONT_UI_PX);
	int32_t pen = x;
	int32_t top = base - (int32_t)f->base_line;
	int32_t max_pen = pen;

	for (uint32_t i = 0; i < 8u * (uint32_t)(len / 2u + 2u); i++) {
		if (p >= end) {
			break;
		}
		uint32_t cp = utf8_next_t(&p);

		if (cp == 0u) {
			break;
		}
		if (cp == '\n') {
			pen = x;
			top += (int32_t)f->line_height;
			base += (int32_t)f->line_height;
			continue;
		}
		lv_font_glyph_dsc_t gd;

		if (!lv_font_get_glyph_dsc(f, &gd, cp, 0)) {
			pen += fpx / 2;
			continue;
		}
		int32_t gw = (int32_t)gd.adv_w;

		if (gd.box_w && gd.box_h) {
			/* A8 mask via the public chain. cpfont returns the
			 * slot's fixed draw buf and leaves release_glyph NULL,
			 * so the release below does not fire in this build --
			 * it is the chain's contract, kept for any resolved
			 * font that owns what it handed back. */
			const lv_font_t *rf = gd.resolved_font ? gd.resolved_font : f;
			lv_draw_buf_t *db = (lv_draw_buf_t *)
				lv_font_get_glyph_bitmap(&gd, NULL);

			if (db && db->data && db->header.w && db->header.h) {
				uint32_t stride = db->header.stride ?
					db->header.stride :
					lv_draw_buf_width_to_stride(db->header.w,
							    LV_COLOR_FORMAT_A8);
				/* LVGL label convention, base ==
				 * line_top + resolved base_line:
				 * y1 = base - box_h - ofs_y */
				int32_t gx0 = pen + gd.ofs_x;
				int32_t gy0 = base - (int32_t)gd.box_h - gd.ofs_y;

				for (uint32_t yy = 0; yy < gd.box_h; yy++) {
					int32_t py = gy0 + (int32_t)yy;

					if (py < 0 || py >= (int32_t)t->h) {
						continue;
					}
					const uint8_t *srow = db->data +
						(size_t)yy * stride;
					uint16_t *drow = row_at(t, (uint32_t)py);
					uint16_t pbg = out_pack(bg);

					for (uint32_t xx = 0; xx < gd.box_w; xx++) {
						int32_t pxx = gx0 + (int32_t)xx;
						uint8_t cov = srow[xx];


						if (pxx < 0 || pxx >= (int32_t)t->w) {
							continue;
						}
						if (cov == 0u) {
							if (has_bg) {
								drow[pxx] = pbg;
							}
							continue;
						}
						drow[pxx] = out_blend(drow[pxx], fg, cov);
					}
				}
				if (rf->release_glyph) {
					rf->release_glyph(rf, &gd);
				}
			} else if (rf->release_glyph) {
				rf->release_glyph(rf, &gd);
			}
		}
		pen += gw;
		if (pen > max_pen) {
			max_pen = pen;
		}
	}
	acc_merge(acc, x < 0 ? 0u : (uint32_t)x,
		  top < 0 ? 0u : (uint32_t)top,
		  max_pen > (int32_t)t->w ? t->w : (uint32_t)max_pen,
		  (uint32_t)(base + ((int32_t)f->line_height - (int32_t)f->base_line)) >
			  t->h ? t->h
			       : (uint32_t)(base + ((int32_t)f->line_height -
						    (int32_t)f->base_line)));
}

/* ---------------- sprite cache ---------------- */

#define SPRITE_SLOTS   6
#define SPRITE_MAX_PX  (160u * 160u)

typedef struct {
	uint16_t *buf;
	uint32_t w, h;
	uint32_t last_use;
	bool used;
} sprite_t;

static sprite_t s_spr[SPRITE_SLOTS];
static uint32_t s_spr_seq;

/* Last rasterized surface, so `aster ui screenshot` can grab game frames that
 * never touch the LVGL screen. Same worker thread writes and reads it. */
static const uint8_t *s_last_buf;
static uint32_t s_last_w, s_last_h, s_last_pitch;
static bool s_last_be;

static int sprite_alloc(uint32_t w, uint32_t h, uint32_t *handle)
{
	uint32_t victim = 0;
	uint32_t oldest = UINT32_MAX;
	bool free_found = false;

	for (uint32_t i = 0; i < SPRITE_SLOTS; i++) {
		if (!s_spr[i].used) {
			victim = i;
			free_found = true;
			break;
		}
		if (s_spr[i].last_use < oldest) {
			oldest = s_spr[i].last_use;
			victim = i;
		}
	}
	(void)free_found;
	sprite_t *sp = &s_spr[victim];

	if (sp->used) {
		aster_psram_free(sp->buf);
		sp->used = false;
	}
	sp->buf = aster_psram_alloc((size_t)w * h * 2u);
	if (!sp->buf) {
		return ASTER_ENOMEM;
	}
	sp->w = w;
	sp->h = h;
	sp->used = true;
	sp->last_use = ++s_spr_seq;
	*handle = victim + 1u;
	return ASTER_OK;
}

int32_t aster_gfx_bmp_load_into(const char *real_path, uint32_t cap_w, uint32_t cap_h,
			      uint32_t *out_handle, uint32_t *out_w, uint32_t *out_h)
{
	aster_imgdec_info_t info;
	aster_error_t e = aster_imgdec_probe(real_path, &info, cap_w, cap_h);

	if (e != ASTER_OK) {
		return e;
	}
	if (info.unsupported || info.too_big) {
		return ASTER_ENOTSUP;
	}
	uint32_t fw = info.out_w ? info.out_w : cap_w;
	uint32_t fh = info.out_h ? info.out_h : cap_h;

	if ((uint64_t)fw * fh > SPRITE_MAX_PX) {
		return ASTER_ENOTSUP; /* ask for a smaller cap */
	}
	uint32_t handle = 0;

	e = sprite_alloc(fw, fh, &handle);
	if (e != ASTER_OK) {
		return e;
	}
	sprite_t *sp = &s_spr[handle - 1u];
	uint32_t ow = 0, oh = 0;

	e = aster_imgdec_render(real_path, (uint8_t *)sp->buf, fw, fh, &ow, &oh, true);
	if (e != ASTER_OK || ow == 0u || oh == 0u) {
		aster_psram_free(sp->buf);
		sp->buf = NULL;
		sp->used = false;
		return e != ASTER_OK ? e : ASTER_ECORRUPT;
	}
	sp->w = ow;
	sp->h = oh;
	*out_handle = handle;
	*out_w = ow;
	*out_h = oh;
	return ASTER_OK;
}

int aster_gfx_bmp_release(uint32_t handle)
{
	if (handle == 0u || handle > SPRITE_SLOTS) {
		return ASTER_EINVAL;
	}
	sprite_t *sp = &s_spr[handle - 1u];

	if (sp->used) {
		aster_psram_free(sp->buf);
		sp->buf = NULL;
		sp->used = false;
	}
	return ASTER_OK;
}

void aster_gfx_drop_all(void)
{
	s_last_buf = NULL;
	for (uint32_t i = 0; i < SPRITE_SLOTS; i++) {
		if (s_spr[i].used) {
			aster_psram_free(s_spr[i].buf);
			s_spr[i].buf = NULL;
			s_spr[i].used = false;
		}
	}
}

static void op_bmp(ppg_target_t *t, const aster_gfx_cmd_t *c, acc_t *acc)
{
	uint32_t op = ASTER_GFX_OP(c);
	int32_t x = (int32_t)cmd_xy0(c), y = (int32_t)cmd_xy1(c);
	uint32_t handle = c->w[2]; /* whole word, not a packed half */
	uint32_t scale = (op == ASTER_GFX_BMP_S || op == ASTER_GFX_BMP_SA)
				 ? (c->w[4] & ASTER_GFX_SCALE_MASK) : 256u;
	uint32_t alpha = (op == ASTER_GFX_BMP_A || op == ASTER_GFX_BMP_SA)
				 ? ((op == ASTER_GFX_BMP_SA
					     ? (c->w[5] & ASTER_GFX_ALPHA_MASK)
					     : (c->w[4] & ASTER_GFX_ALPHA_MASK)))
				 : 255u;

	if (handle == 0u || handle > SPRITE_SLOTS || scale < 8u) {
		return;
	}
	sprite_t *sp = &s_spr[handle - 1u];

	if (!sp->used) {
		return;
	}
	sp->last_use = ++s_spr_seq;
	if (alpha == 0u) {
		return;
	}
	uint32_t dw = (uint32_t)((uint64_t)sp->w * scale / 256u);
	uint32_t dh = (uint32_t)((uint64_t)sp->h * scale / 256u);

	if (!dw || !dh) {
		return;
	}
	{
		uint32_t cx, cy, cw, ch;

		if (clip_rect(t, x, y, dw, dh, &cx, &cy, &cw, &ch)) {
			acc_merge(acc, cx, cy, cx + cw, cy + ch);
		}
	}
	for (uint32_t py = 0; py < dh; py++) {
		int32_t ty = y + (int32_t)py;

		if (ty < 0 || ty >= (int32_t)t->h) {
			continue;
		}
		uint32_t sy = (uint32_t)((uint64_t)py * sp->h / dh);
		const uint16_t *srow = sp->buf + (size_t)sy * sp->w;
		uint16_t *drow = row_at(t, (uint32_t)ty);

		for (uint32_t px = 0; px < dw; px++) {
			int32_t tx2 = x + (int32_t)px;

			if (tx2 < 0 || tx2 >= (int32_t)t->w) {
				continue;
			}
			uint32_t sx2 = (uint32_t)((uint64_t)px * sp->w / dw);
			uint16_t col = srow[sx2];

			if (col == 0u) {
				continue; /* color key */
			}
			drow[tx2] = (alpha >= 255u) ? out_pack(col)
						    : out_blend(drow[tx2], col, alpha);
		}
	}
}

/* ---------------- public entry ---------------- */

bool aster_gfx_last_frame(const uint8_t **buf, uint32_t *w, uint32_t *h,
			uint32_t *pitch, bool *out_be)
{
	if (!s_last_buf) {
		return false;
	}
	*buf = s_last_buf;
	*w = s_last_w;
	*h = s_last_h;
	*pitch = s_last_pitch;
	if (out_be) {
		*out_be = s_last_be;
	}
	return true;
}

#if IS_ENABLED(CONFIG_ASTER_GFX_TIMING)
/* Per-op raster time histogram. Kept here (not in the app) because the rasterization
 * runs inside aster_gfx_exec(): only the firmware can attribute cost per op. */
#define GFX_OP_BUCKETS 16u
static uint32_t s_op_cyc[GFX_OP_BUCKETS];
static uint32_t s_op_n[GFX_OP_BUCKETS];

static void gfx_op_note(uint32_t op, uint32_t cyc)
{
	if (op < GFX_OP_BUCKETS) {
		s_op_cyc[op] += cyc;
		s_op_n[op]++;
	}
}

/* Indexed by opcode, so slot 8 must stay a slot even though nothing
 * dispatches it (see the reserved-op note in abi/include/aster/gfx.h). The
 * name says "reserved" rather than being dropped so the histogram stays
 * aligned with the op numbers, and so a nonzero count here is immediately
 * readable as "an app sent the reserved op". */
static const char *const gfx_op_name[GFX_OP_BUCKETS] = {
	"CLEAR", "RECT_F", "RECT_S", "LINE_AA", "ELLIPSE", "TRI", "GRAD",
	"TEXT", "reserved", "BMP", "BMP_S", "BMP_A", "BMP_SA",
};

/* The 64-frame timing dump (the aggregate "gfx tim:" line plus this
 * histogram) is formatted and written from its own thread, not the caller's:
 * the caller is the app's render thread, whose frame (raster ~55ms + flush
 * ~11ms out of a 69ms sample period) leaves ~3ms of slack, while a
 * synchronous console line costs milliseconds - the burst every 64 frames
 * was enough to miss the deadline and show as a periodic hitch. The counters
 * are the render thread's state, so the snapshot is taken there; only
 * formatting and the console writes move. The worker sits below the app
 * thread (preempt 7) and above idle, so it runs in the app's inter-frame
 * sleep and never delays a frame. */
static struct k_work_q s_tim_wq;
static struct k_work s_tim_work;
static uint32_t s_tim_snap[6]; /* ras, flu, ras_max, flu_max, cmds, px */
static uint32_t s_op_snap_cyc[GFX_OP_BUCKETS];
static uint32_t s_op_snap_n[GFX_OP_BUCKETS];

static void gfx_timing_dump(struct k_work *work)
{
	uint32_t total = 0;

	ARG_UNUSED(work);
	LOG_INF("gfx tim: raster=%u us flush=%u us sum=%u us "
		"(max %u/%u) cmds=%u flushed=%u px",
		(unsigned)s_tim_snap[0], (unsigned)s_tim_snap[1],
		(unsigned)(s_tim_snap[0] + s_tim_snap[1]),
		(unsigned)s_tim_snap[2], (unsigned)s_tim_snap[3],
		(unsigned)s_tim_snap[4], (unsigned)s_tim_snap[5]);

	for (uint32_t i = 0; i < GFX_OP_BUCKETS; i++) {
		total += s_op_snap_cyc[i];
	}
	if (!total) {
		return;
	}
	/* per-op us/frame = cycles / 64 frames, then to us */
	for (uint32_t i = 0; i < GFX_OP_BUCKETS; i++) {
		if (!s_op_snap_n[i]) {
			continue;
		}
		uint32_t us = k_cyc_to_us_floor32(s_op_snap_cyc[i]) / 64u;
		uint32_t pct = (uint32_t)((uint64_t)s_op_snap_cyc[i] * 100u / total);

		if (us >= 100u || pct >= 3u) {
			LOG_INF("gfx op: %-8s %5u us/frame %3u%%  n=%u",
				gfx_op_name[i], (unsigned)us, (unsigned)pct,
				(unsigned)(s_op_snap_n[i] / 64u));
		}
	}
}

static int gfx_timing_wq_init(void)
{
	k_thread_stack_t *stack = aster_dram_alloc(K_KERNEL_STACK_LEN(2048));

	k_work_queue_init(&s_tim_wq);
	if (stack == NULL) {
		LOG_ERR("gfx timing stack alloc failed");
		return -ENOMEM;
	}
	k_work_queue_start(&s_tim_wq, stack,
			   K_KERNEL_STACK_LEN(2048) - K_KERNEL_STACK_RESERVED,
			   K_PRIO_PREEMPT(10), NULL);
	k_thread_name_set(&s_tim_wq.thread, "gfx_tim");
	k_work_init(&s_tim_work, gfx_timing_dump);
	return 0;
}
SYS_INIT(gfx_timing_wq_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* Snapshot the histogram and the aggregate numbers (both are the render
 * thread's state), then clear them; the worker prints from the snapshot. */
void aster_gfx_timing_report(uint32_t raster_us, uint32_t flush_us,
			     uint32_t raster_max_us, uint32_t flush_max_us,
			     uint32_t cmds, uint32_t px)
{
	s_tim_snap[0] = raster_us;
	s_tim_snap[1] = flush_us;
	s_tim_snap[2] = raster_max_us;
	s_tim_snap[3] = flush_max_us;
	s_tim_snap[4] = cmds;
	s_tim_snap[5] = px;
	memcpy(s_op_snap_cyc, s_op_cyc, sizeof(s_op_cyc));
	memcpy(s_op_snap_n, s_op_n, sizeof(s_op_n));
	memset(s_op_cyc, 0, sizeof(s_op_cyc));
	memset(s_op_n, 0, sizeof(s_op_n));
	k_work_submit(&s_tim_work);
}
#endif /* CONFIG_ASTER_GFX_TIMING */

void aster_gfx_set_clip(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	s_clip_x = x;
	s_clip_y = y;
	s_clip_w = w;
	s_clip_h = h;
}

int aster_gfx_exec_into(uint8_t *buf565, uint32_t w, uint32_t h, uint32_t pitch,
		      const aster_gfx_cmd_t *cmds, uint32_t n,
		      const uint8_t *arena, uint32_t arena_len,
		      uint32_t *dirty_x, uint32_t *dirty_y, uint32_t *dirty_w,
		      uint32_t *dirty_h, bool out_be)
{
	/* Commands paint only their own pixels; the accumulator is the
	 * dirty box the present uses. A panel canvas (out_be) still has to
	 * repaint every pixel it will show, because its front buffer holds
	 * an older frame. */
	ppg_target_t t = { .px = (uint16_t *)buf565, .w = w, .h = h, .pitch = pitch };
	acc_t acc = { 0 };

	set_out_be(out_be);
	if (out_be) {
		s_clip_x = s_clip_y = s_clip_w = s_clip_h = 0;
	}

	s_last_buf = buf565;
	s_last_w = w;
	s_last_h = h;
	s_last_pitch = pitch;
	s_last_be = out_be;

	/* advance the async glyph pipeline: fills land in the ring so the
	 * second frame shows the real bitmap (white-box protocol) */
	(void)aster_font_sd_poll();
	(void)aster_font_sd_commit();
	/* ...and whatever landed is in this frame, so the guest's next
	 * aster_gfx_fonts_pending() may answer "no" again. (The draw is what
	 * clears the request -- see the note on repaint_ack.) */
	aster_font_sd_repaint_ack();

	for (uint32_t i = 0; i < n; i++) {
		const aster_gfx_cmd_t *c = &cmds[i];
#if IS_ENABLED(CONFIG_ASTER_GFX_TIMING)
		uint32_t t_op0 = k_cycle_get_32();
		uint32_t bucket = ASTER_GFX_OP(c);
#endif

		switch (ASTER_GFX_OP(c)) {
		case ASTER_GFX_CLEAR:
			op_clear(&t, c, &acc);
			break;
		case ASTER_GFX_RECT_F:
			op_rect(&t, c, &acc);
			break;
		case ASTER_GFX_RECT_S:
			op_rect_round(&t, c, &acc);
			break;
		case ASTER_GFX_LINE_AA:
			op_line(&t, c, &acc);
			break;
		case ASTER_GFX_ELLIPSE:
			op_ellipse(&t, c, &acc);
			break;
		case ASTER_GFX_TRI:
			op_tri(&t, c, &acc);
			break;
		case ASTER_GFX_GRAD:
			op_grad(&t, c, &acc);
			break;
		case ASTER_GFX_TEXT:
			op_text(&t, c, arena, arena_len, &acc);
			break;
		case ASTER_GFX_BMP:
		case ASTER_GFX_BMP_S:
		case ASTER_GFX_BMP_A:
		case ASTER_GFX_BMP_SA:
			op_bmp(&t, c, &acc);
			break;
		default:
			break;
		}
#if IS_ENABLED(CONFIG_ASTER_GFX_TIMING)
		gfx_op_note(bucket, k_cycle_get_32() - t_op0);
#endif
	}

	if (dirty_x) {
		if (!acc.has) {
			*dirty_x = *dirty_y = *dirty_w = *dirty_h = 0;
		} else {
			uint32_t x1 = acc.x1 > w ? w : acc.x1;
			uint32_t y1 = acc.y1 > h ? h : acc.y1;

			if (x1 <= acc.x0 || y1 <= acc.y0) {
				*dirty_x = *dirty_y = *dirty_w = *dirty_h = 0;
			} else {
				*dirty_x = acc.x0;
				*dirty_y = acc.y0;
				*dirty_w = x1 - acc.x0;
				*dirty_h = y1 - acc.y0;
			}
		}
	}
	return (int)n;
}
