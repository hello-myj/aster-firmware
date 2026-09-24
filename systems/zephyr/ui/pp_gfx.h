/* SPDX-License-Identifier: MIT
 * Anti-aliased polygon path renderer for the ASTER gfx engine, backed by
 * pretty-poly (tile-based exact-coverage supersampling, MIT, vendored at
 * firmware/modules/prettypoly/pretty-poly.h).
 *
 * Fills any convex closed polygon (samples of rounded-rect / ellipse
 * outlines, triangles) with 4x supersampled edge feathering, and can
 * stroke the same path as a constant-width ring (outer contour + inward
 * offset inner contour, even-odd) so outlines never tear between scan
 * rows -- the failure mode of the old endpoint-walking stroker.
 *
 * Single-thread contract identical to ui_gfx.c: the app worker thread is
 * the only caller.
 */
#ifndef ASTER_PP_GFX_H
#define ASTER_PP_GFX_H

#include <stdint.h>
#include <stdbool.h>

/* The RGB565 target descriptor: defined once, in ui_gfx_internal.h, and
 * used by both this renderer and the command engine (ui_gfx.c). */
#include "ui_gfx_internal.h"

/* n >= 3 vertices as packed (x, y) pairs. stroke_w > 0 renders a ring of
 * that width inward from the contour (even-odd: the hole punches itself,
 * winding order irrelevant); 0 fills. Color is RGB565. On success the
 * touched pixel bounds are written to the bx0, by0, bx1, by1 out-params
 * (exclusive, clipped to the target) -- the caller merges them into its
 * dirty acc. Returns 0 or a negative errno-ish value.
 *
 * `clip` bounds the render (NULL = the whole target). Callers use it to
 * keep the expensive supersampled path on the pixels that actually need
 * feathering: a rounded rect's straight-sided middle rows are exact
 * integer spans and are far cheaper to fill directly, and its four corner
 * arcs live in four r x r squares rather than across the full width (see
 * op_rect_round). pretty-poly prunes both the tile walk and the per-tile
 * rasterization against this rect, so a small clip is proportionally
 * cheap instead of paying for the whole shape. */
#define PP_MAX_VERTS 64

typedef struct {
	int32_t x0, y0, x1, y1; /* half-open */
} ppg_clip_t;

int ppg_path(const ppg_target_t *t, const float *xy, int n, float stroke_w,
	     uint16_t col, const ppg_clip_t *clip, int32_t *bx0, int32_t *by0,
	     int32_t *bx1, int32_t *by1);

/* Byte order of `t->px` for the next ppg_path calls: false for an
 * app-visible ABI RGB565 surface, true for a panel-order canvas, in which
 * case the swizzle is folded into the blend instead of costing a pass over
 * the finished frame. Mirrors the flag of the same name in ui_gfx.c: the
 * engine's set_out_be() is the single write point for the pair. */
void ppg_set_out_be(bool out_be);

#endif /* ASTER_PP_GFX_H */
