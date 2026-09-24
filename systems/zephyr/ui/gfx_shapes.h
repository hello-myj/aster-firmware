/* SPDX-License-Identifier: MIT
 * Primitive -> polygon sampling for the gfx engine's antialiased path.
 *
 * These two builders are shared verbatim by the firmware rasterizer
 * (ui_gfx.c) and by the host-side shape preview, so what the preview shows
 * is what the device draws. Keep them free of firmware/Zephyr deps.
 *
 * Screen coordinates are y-down. Angles are the usual (cos, sin) with +y
 * pointing DOWN, so th=pi/2 is the BOTTOM of a circle and th=3pi/2 the top.
 */
#ifndef ASTER_GFX_SHAPES_H
#define ASTER_GFX_SHAPES_H

#include <math.h>
#include <stdint.h>

/* Max vertices either builder emits (rounded rect: 4 arcs * (k+1), k<=8). */
#define ASTER_GFX_SHAPE_MAXV 36

/* Two contour points closer than this are the same point. The tangency
 * duplicates a rounded rect emits when `2*r == w|h` differ by ~1e-6 from
 * float rounding, not exactly, so an exact test leaves a zero-ish length
 * edge behind and the stroker's miter normals divide by that noise. */
#define ASTER_GFX_EPS 0.01f

static inline int aster_gfx_same_point(float ax, float ay, float bx, float by)
{
	float dx = ax - bx, dy = ay - by;

	return (dx * dx + dy * dy) < (ASTER_GFX_EPS * ASTER_GFX_EPS);
}

/* Rounded rect `x,y,w,h` with corner radius `r` -> closed contour of `n`
 * vertices written as packed (x,y) pairs into xy[2*n]. Winding does not
 * matter to the renderer (even-odd), but this is clockwise on screen:
 * top edge -> TR arc -> right edge -> BR arc -> bottom -> BL arc -> left
 * -> TL arc, each arc sweeping pi/2 from the edge tangent point. */
static inline uint32_t aster_gfx_rr_polygon(float *xy, int32_t x, int32_t y,
					  uint32_t w, uint32_t h, uint32_t r)
{
	/* 2..8 segments per corner arc: more for bigger radii. */
	uint32_t k = 2u + r / 6u;

	if (k > 8u) {
		k = 8u;
	}
	/* A radius past half the short side is not a rounded rect any more
	 * (the flat edge between two corners vanishes and the arcs start
	 * crossing); clamping is what CSS and every other renderer does. */
	if (r > (w < h ? w : h) / 2u) {
		r = (w < h ? w : h) / 2u;
	}
	uint32_t n = 0;
	const float pi2 = 6.2831853f;

	for (uint32_t side = 0; side < 4u; side++) {
		/* Corner centers in TL, TR, BR, BL order. */
		float cxr = (side == 0u || side == 3u) ? (float)(x + (int32_t)r)
						       : (float)(x + (int32_t)w - (int32_t)r);
		float cyr = (side < 2u) ? (float)(y + (int32_t)r)
					: (float)(y + (int32_t)h - (int32_t)r);
		/* Start angle per corner. Each arc must begin at the point where
		 * the incoming straight edge meets the circle:
		 *   TL pi   (left edge  -> top edge)
		 *   TR 3pi/2(top edge  -> right edge)
		 *   BR 2pi  (right     -> bottom)
		 *   BL 5pi/2(bottom    -> left)
		 * i.e. pi + side*pi/2. Getting the constant wrong here rotates
		 * every corner by 90 degrees and the rect comes out malformed. */
		float a0 = (float)side * (pi2 / 4.0f) + 3.14159265f;

		for (uint32_t i = 0; i <= k; i++) {
			float th = a0 + (pi2 / 4.0f) * (float)i / (float)k;
			float vx = cxr + (float)r * cosf(th);
			float vy = cyr + (float)r * sinf(th);

			/* Adjacent arcs meet at their shared tangent point when
			 * 2*r == w|h (the flat edge has zero length). Emitting it
			 * twice would hand the stroker a zero-length edge, which
			 * has no direction for the miter normal: drop duplicates. */
			if (n > 0u && aster_gfx_same_point(xy[2 * n - 2],
							 xy[2 * n - 1], vx, vy)) {
				continue;
			}
			xy[2 * n] = vx;
			xy[2 * n + 1] = vy;
			n++;
		}
	}
	/* the contour closes implicitly, so a trailing copy of the first
	 * vertex is a zero-length edge as well */
	if (n > 3u && aster_gfx_same_point(xy[2 * n - 2], xy[2 * n - 1], xy[0],
					 xy[1])) {
		n--;
	}
	return n;
}

/* Axis-aligned ellipse -> closed contour of `n` vertices. Segment count
 * scales with the mean radius so small dots stay cheap and large rings do
 * not facet; the renderer's supersampling handles the remaining error. */
static inline uint32_t aster_gfx_ell_polygon(float *xy, int32_t ccx, int32_t ccy,
					   uint32_t rx, uint32_t ry)
{
	const float pi2 = 6.2831853f;
	uint32_t n = 16u + (rx + ry) / 3u;

	if (n > 60u) {
		n = 60u;
	}
	for (uint32_t i = 0; i < n; i++) {
		float th = pi2 * (float)i / (float)n;

		xy[2 * i] = (float)ccx + (float)rx * cosf(th);
		xy[2 * i + 1] = (float)ccy + (float)ry * sinf(th);
	}
	return n;
}

#endif /* ASTER_GFX_SHAPES_H */
