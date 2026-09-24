/* SPDX-License-Identifier: MIT
 * pretty-poly glue for the ASTER gfx engine (see pp_gfx.h).
 *
 * pretty-poly is vendored at firmware/modules/prettypoly/ with a
 * single-TU static patch; this file is the ONLY translation unit that
 * defines PP_IMPLEMENTATION.
 *
 * Allocation: pp_poly/pp_path live in a 2 KB bump arena, reset at the end
 * of every ppg_path() call -- one render graph at a time, and no
 * realloc-chaining against Zephyr's small system heap. Free is a no-op.
 * Path vertex buffers are filled by hand (exact size up front) because
 * the arena cannot grow a buffer in place.
 */
#include <errno.h>
#include <math.h>
#include <string.h>

#include "pp_gfx.h"
#include "gfx_shapes.h" /* aster_gfx_same_point: degenerate-edge test */
/* relative path: the host preview compiles this file with -I pointing only
 * at firmware/ui (see hostpreview/preview.c), so a bare <core/> name would
 * not resolve there. Same header the command engine blends with. */
#include "../core/aster_color565.h"

/* ---------------- arena (must precede PP_IMPLEMENTATION) ---------------- */

#define PP_ARENA_BYTES 2048u

static uint8_t s_arena_buf[PP_ARENA_BYTES] __attribute__((aligned(8)));
static uint32_t s_arena_used;

static void *pp_alloc_hook(size_t size)
{
	size = (size + 7u) & ~(size_t)7u;
	if (s_arena_used + size > PP_ARENA_BYTES) {
		return NULL;
	}
	void *p = s_arena_buf + s_arena_used;

	s_arena_used += (uint32_t)size;
	return p;
}

/* pretty-poly only ever mallocs/reallocs (both hooked below); PP_CALLOC is
 * defined but never called, so it gets no hook of its own. */
#define PP_MALLOC(size)     pp_alloc_hook(size)
#define PP_REALLOC(p, s)    NULL /* never: we pre-size every path */
#define PP_FREE(p)          ((void)(p))

#define PP_IMPLEMENTATION
#include "pretty-poly.h"

/* ---------------- 565 blend ---------------- */

/* Portable byte swap: this file is also linked into the host preview tool,
 * which has no Zephyr headers (contrast ui_gfx.c, which includes
 * zephyr/sys/byteorder.h). The compiler turns this into a rev16. */
static inline uint16_t pp_bswap16(uint16_t v)
{
	return (uint16_t)((uint16_t)(v << 8) | (uint16_t)(v >> 8));
}

/* ch_up/blend565 now live in core/aster_color565.h (the copy in ui_gfx.c was
 * identical, so it became the shared one). */

/* ---------------- render state (single renderer contract) ---------------- */

static const ppg_target_t *s_t;
static uint16_t s_col;
static int32_t s_bx0, s_by0, s_bx1, s_by1;
static int32_t s_cx0, s_cy0, s_cx1, s_cy1; /* render window, half-open */
static bool s_out_be;        /* target holds panel-order words */

/* Coverage-weighted 565 blend into the surface, alpha = tile coverage. */
static void tile_cb(const pp_tile_t *tile)
{
	int32_t ty0 = tile->y, tx0 = tile->x;
	int32_t ty1 = ty0 + tile->h, tx1 = tx0 + tile->w;

	if (ty0 < s_cy0) {
		ty0 = s_cy0;
	}
	if (ty1 > s_cy1) {
		ty1 = s_cy1;
	}
	if (tx0 < s_cx0) {
		tx0 = s_cx0;
	}
	if (tx1 > s_cx1) {
		tx1 = s_cx1;
	}

	if (ty0 < 0) {
		ty0 = 0;
	}
	if (tx0 < 0) {
		tx0 = 0;
	}
	if (ty1 > (int32_t)s_t->h) {
		ty1 = (int32_t)s_t->h;
	}
	if (tx1 > (int32_t)s_t->w) {
		tx1 = (int32_t)s_t->w;
	}
	for (int32_t y = ty0; y < ty1; y++) {
		const uint8_t *src = tile->data +
			(size_t)(y - tile->y) * tile->stride + (size_t)(tx0 - tile->x);
		uint16_t *dst = s_t->px + (size_t)y * s_t->pitch + (size_t)tx0;
		uint16_t pc = s_out_be ? pp_bswap16(s_col) : s_col;

		for (int32_t x = tx0; x < tx1; x++) {
			uint8_t cov = *src++;
			uint16_t *o = dst + (x - tx0);

			if (!cov) {
				continue;
			}
			if (cov >= 255u) {
				*o = pc;
			} else if (s_out_be) {
				/* blend in ABI order, store panel order: the
				 * swizzle is free here (it rides along with the
				 * value the blend already has to compute) whereas
				 * a post-pass over the frame costs ~7 ms. */
				*o = pp_bswap16(blend565(pp_bswap16(*o),
							 s_col, cov));
			} else {
				*o = blend565(*o, s_col, cov);
			}
		}
	}
	/* tiles with a nonzero callback still include fully-empty rows at
	 * their edges; widening the bounds by a tile is harmless (dirty
	 * rects are merged per frame anyway) so we record the tile box. */
	if (tx0 < s_bx0) {
		s_bx0 = tx0;
	}
	if (ty0 < s_by0) {
		s_by0 = ty0;
	}
	if (tx1 > s_bx1) {
		s_bx1 = tx1;
	}
	if (ty1 > s_by1) {
		s_by1 = ty1;
	}
}

/* ---------------- path building ---------------- */

/* One closed contour of `xy`, optionally inset by `inset` px along the
 * (mitered, centroid-facing) vertex normals. stroke rings are built as
 * outer contour + inset inner contour; even-odd fill punches the hole.
 * `xy` must already be compacted (no zero-length edges). */
static int add_inset_path(pp_poly_t *poly, const float *xy, int n, float inset)
{
	pp_path_t *path = pp_poly_add_path(poly);
	pp_point_t *pts;

	if (!path) {
		return -ENOMEM;
	}
	/* pp_poly_add_path reserved 8 slots we don't use (the arena reuses
	 * them until the end-of-render reset); take an exact slice. */
	pts = (pp_point_t *)PP_MALLOC(sizeof(pp_point_t) * (uint32_t)n);
	if (!pts) {
		return -ENOMEM;
	}
	path->points = pts;
	path->storage = n;
	path->count = 0;

	if (inset <= 0.0f) {
		for (int i = 0; i < n; i++) {
			pts[i].x = xy[2 * i];
			pts[i].y = xy[2 * i + 1];
		}
		path->count = n;
		return 0;
	}

	/* centroid tells us which way is "in" (contours are winding-agnostic) */
	float cx = 0.0f, cy = 0.0f;

	for (int i = 0; i < n; i++) {
		cx += xy[2 * i];
		cy += xy[2 * i + 1];
	}
	cx /= (float)n;
	cy /= (float)n;

	for (int i = 0; i < n; i++) {
		int prev = (i + n - 1) % n, next = (i + 1) % n;
		float ax = xy[2 * prev], ay = xy[2 * prev + 1];
		float bx = xy[2 * i], by = xy[2 * i + 1];
		float dx2 = xy[2 * next], dy2 = xy[2 * next + 1];
		float e1x = bx - ax, e1y = by - ay;
		float e2x = dx2 - bx, e2y = dy2 - by;
		float l1 = sqrtf(e1x * e1x + e1y * e1y);
		float l2 = sqrtf(e2x * e2x + e2y * e2y);

		if (l1 <= 0.0f || l2 <= 0.0f) {
			/* only reachable for hand-built contours that skipped the
			 * compaction pass; no tangent, so no ring for this shape */
			return -EINVAL;
		}
		float n1x = -e1y / l1, n1y = e1x / l1;
		float n2x = -e2y / l2, n2y = e2x / l2;
		float nx = n1x + n2x, ny = n1y + n2y;
		float nl = sqrtf(nx * nx + ny * ny);

		if (nl <= 0.0001f) {
			nx = n1x;
			ny = n1y;
		} else {
			nx /= nl;
			ny /= nl;
		}
		if ((cx - bx) * nx + (cy - by) * ny < 0.0f) {
			nx = -nx;
			ny = -ny;
		}
		/* miter length; capped at 4x so needle corners stay sane */
		float cos_half = nx * n1x + ny * n1y;

		if (cos_half < 0.0f) {
			cos_half = -cos_half;
		}
		if (cos_half < 0.25f) {
			cos_half = 0.25f;
		}
		pts[i].x = bx + nx * (inset / cos_half);
		pts[i].y = by + ny * (inset / cos_half);
	}
	path->count = n;
	return 0;
}

/* ---------------- public ---------------- */

/* Drop duplicated consecutive vertices (including the wrap-around pair that
 * closes the contour). A rounded rect whose corner arcs meet exactly, i.e.
 * 2*r == w|h, emits its shared tangent point twice (to within float rounding);
 * a zero-length edge has no direction, so the stroker's miter normals would
 * divide by noise. Returns the compacted count and the contour bbox. */
static int compact_contour(const float *xy, int n, float *out, float *x0,
			   float *y0, float *x1, float *y1)
{
	int m = 0;

	*x0 = *x1 = xy[0];
	*y0 = *y1 = xy[1];
	for (int i = 0; i < n; i++) {
		float px = xy[2 * i], py = xy[2 * i + 1];

		if (px < *x0) *x0 = px;
		if (px > *x1) *x1 = px;
		if (py < *y0) *y0 = py;
		if (py > *y1) *y1 = py;
		if (m > 0 && aster_gfx_same_point(out[2 * m - 2], out[2 * m - 1],
						px, py)) {
			continue;
		}
		out[2 * m] = px;
		out[2 * m + 1] = py;
		m++;
	}
	if (m > 3 && aster_gfx_same_point(out[2 * m - 2], out[2 * m - 1], out[0],
					out[1])) {
		m--;
	}
	return m;
}

int ppg_path(const ppg_target_t *t, const float *xy, int n, float stroke_w,
	     uint16_t col, const ppg_clip_t *clip, int32_t *bx0, int32_t *by0,
	     int32_t *bx1, int32_t *by1)
{
	float cxy[PP_MAX_VERTS * 2];
	float x0, y0, x1, y1;
	int m;
	int32_t cx0, cy0, cx1, cy1;

	if (!t || !t->px || !xy || n < 3 || n > PP_MAX_VERTS) {
		return -EINVAL;
	}
	if (stroke_w < 0.0f) {
		stroke_w = 0.0f;
	}
	if (stroke_w > 64.0f) {
		stroke_w = 64.0f;
	}
	/* Render window, clamped to the target in both directions so a caller
	 * cannot widen the render by passing a bogus rect. */
	cx0 = clip ? clip->x0 : 0;
	cy0 = clip ? clip->y0 : 0;
	cx1 = clip ? clip->x1 : (int32_t)t->w;
	cy1 = clip ? clip->y1 : (int32_t)t->h;
	if (cx0 < 0) {
		cx0 = 0;
	}
	if (cy0 < 0) {
		cy0 = 0;
	}
	if (cx1 > (int32_t)t->w) {
		cx1 = (int32_t)t->w;
	}
	if (cy1 > (int32_t)t->h) {
		cy1 = (int32_t)t->h;
	}
	if (cx0 >= cx1 || cy0 >= cy1) {
		return -EINVAL;
	}
	if (s_t != NULL) {
		return -EBUSY; /* renderer reentrancy contract */
	}
	m = compact_contour(xy, n, cxy, &x0, &y0, &x1, &y1);
	if (m < 3) {
		return -EINVAL; /* zero-area contour */
	}

	pp_poly_t *poly = (pp_poly_t *)PP_MALLOC(sizeof(pp_poly_t));

	if (!poly) {
		return -ENOMEM;
	}
	memset(poly, 0, sizeof(*poly));

	int err = add_inset_path(poly, cxy, m, 0.0f);

	/* The ring is a second, inward-offset contour that even-odd punches
	 * out of the fill. A shape too thin to hold a hole (< 2 strokes wide)
	 * is left solid: an inverted/self-crossing inner contour would punch
	 * arbitrary wedges out of it instead. */
	if (!err && stroke_w > 0.5f && (x1 - x0) > 2.0f * stroke_w &&
	    (y1 - y0) > 2.0f * stroke_w) {
		err = add_inset_path(poly, cxy, m, stroke_w);
	}
	if (err) {
		s_arena_used = 0;
		return err;
	}

	s_t = t;
	s_col = col;
	s_cx0 = cx0;
	s_cy0 = cy0;
	s_cx1 = cx1;
	s_cy1 = cy1;
	s_bx0 = (int32_t)t->w;
	s_by0 = (int32_t)t->h;
	s_bx1 = 0;
	s_by1 = 0;
	pp_tile_callback(tile_cb);
	pp_antialias(PP_AA_X4);
	pp_clip(cx0, cy0, cx1 - cx0, cy1 - cy0);
	pp_render(poly);
	pp_tile_callback(NULL);
	s_t = NULL;
	s_cx0 = 0;
	s_cy0 = 0;
	s_cx1 = (int32_t)t->w;
	s_cy1 = (int32_t)t->h;

	if (s_bx1 > s_bx0 && s_by1 > s_by0 && bx0) {
		*bx0 = s_bx0;
		*by0 = s_by0;
		*bx1 = s_bx1;
		*by1 = s_by1;
	}
	/* pp_poly_free walks PP_FREE (a no-op here); the reset is the GC */
	s_arena_used = 0;
	return 0;
}

/* The pp-side write point for the output byte order. It is only ever called
 * from ui_gfx.c's set_out_be(), which is the engine's single write point for
 * the flag pair; standalone host builds can call it directly. */
void ppg_set_out_be(bool out_be)
{
	s_out_be = out_be;
}
