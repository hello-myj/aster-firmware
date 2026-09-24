/* Host-side shape preview for the ASTER gfx engine's AA path.
 *
 * Links the real firmware sources (pp_gfx.c + pp_gfx.h) against a tiny
 * main(), so what this renders is byte-for-byte what the device rasterizes.
 * Used to verify rounded-rect / ellipse contours and their stroke rings
 * without a board attached.
 *
 *   cc -O2 -I<aster>/firmware/ui -I<aster>/firmware/modules/prettypoly \
 *      -o preview preview.c <aster>/firmware/ui/pp_gfx.c -lm
 *   ./preview out.raw
 *   python3 raw2png.py out.raw out.png <W> <H>
 *   python3 verify_shapes.py out.raw <W> <H>
 *
 * The canvas is 240 wide (the panel width) but taller than 320 so the fill
 * and ring rows have a real gap between them: verification regions must not
 * catch a neighbouring shape's pixels.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pp_gfx.h"
#include "gfx_shapes.h"

#define W 240
#define H 360

static uint16_t fb[W * H];

static const uint16_t WHITE = 0xFFFF;
static const uint16_t ACC = 0x07E0;         /* green */
static const uint16_t ACC2 = 0xF800;        /* red */
static const uint16_t ACC3 = 0x001F;        /* blue */
static const uint16_t ACC4 = 0xFFE0;        /* yellow */

static ppg_target_t T;

static void clear(void)
{
	memset(fb, 0, sizeof(fb));
}

/* Straight span fill, mirroring ui_gfx.c's fill_span(). */
static void span(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t col)
{
	if (x0 < 0) {
		x0 = 0;
	}
	if (y0 < 0) {
		y0 = 0;
	}
	if (x1 > W) {
		x1 = W;
	}
	if (y1 > H) {
		y1 = H;
	}
	if (x0 >= x1 || y0 >= y1) {
		return;
	}
	for (int32_t yy = y0; yy < y1; yy++) {
		uint16_t *row = fb + (size_t)yy * W;

		for (int32_t i = x0; i < x1; i++) {
			row[i] = col;
		}
	}
}

/* Mirror of ui_gfx.c's op_rect_round: supersample only the four corner
 * squares, fill the axis-aligned body/ring directly. Kept in lockstep with
 * the firmware so this harness verifies what the device renders. */
static int draw_rr(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t r,
		   uint32_t outline, uint16_t col)
{
	float xy[ASTER_GFX_SHAPE_MAXV * 2u];
	uint32_t n = aster_gfx_rr_polygon(xy, x, y, w, h, r);
	int rc = 0;

	if (n >= 3u && r >= 2u && w >= 2u && h >= 2u) {
		int32_t R = (int32_t)r;
		int32_t x1 = x + (int32_t)w, y1 = y + (int32_t)h;
		const struct {
			int32_t x0, y0, x1, y1;
		} corners[4] = {
			{ x, y, x + R, y + R },
			{ x1 - R, y, x1, y + R },
			{ x, y1 - R, x + R, y1 },
			{ x1 - R, y1 - R, x1, y1 },
		};

		for (int i = 0; i < 4; i++) {
			ppg_clip_t cl = { corners[i].x0, corners[i].y0,
					  corners[i].x1, corners[i].y1 };
			int32_t bx0, by0, bx1, by1;

			if (ppg_path(&T, xy, (int)n, outline ? 1.0f : 0.0f, col,
				     &cl, &bx0, &by0, &bx1, &by1) != 0) {
				return -1;
			}
		}
		if (!outline) {
			span(x + R, y, x1 - R, y + R, col);
			span(x, y + R, x1, y1 - R, col);
			span(x + R, y1 - R, x1 - R, y1, col);
		} else {
			span(x + R, y, x1 - R, y + 1, col);
			span(x + R, y1 - 1, x1 - R, y1, col);
			span(x, y + R, x + 1, y1 - R, col);
			span(x1 - 1, y + R, x1, y1 - R, col);
		}
		return rc;
	}
	if (outline) {
		for (uint32_t yy = 0; yy < h; yy++) {
			uint16_t *row = fb + (size_t)(y + (int32_t)yy) * W;

			for (uint32_t i = 0; i < w; i++) {
				if (yy == 0 || yy + 1u == h || i == 0 || i + 1u == w) {
					row[x + (int32_t)i] = col;
				}
			}
		}
	} else {
		for (uint32_t yy = 0; yy < h; yy++) {
			uint16_t *row = fb + (size_t)(y + (int32_t)yy) * W;

			for (uint32_t i = 0; i < w; i++) {
				row[x + (int32_t)i] = col;
			}
		}
	}
	return rc;
}

static int draw_ell(int32_t cx, int32_t cy, uint32_t rx, uint32_t ry,
		    uint32_t stroke, uint16_t col)
{
	float xy[64 * 2u];
	uint32_t n = aster_gfx_ell_polygon(xy, cx, cy, rx, ry);
	int32_t bx0, by0, bx1, by1;

	return ppg_path(&T, xy, (int)n, stroke ? 1.5f : 0.0f, col, NULL,
			&bx0, &by0, &bx1, &by1);
}

/* Contour dump: verifies the vertex order/geometry numerically, which is
 * how the -90 degree corner bug in rr_polygon was found. */
static void dump_rr(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t r)
{
	float xy[ASTER_GFX_SHAPE_MAXV * 2u];
	uint32_t n = aster_gfx_rr_polygon(xy, x, y, w, h, r);
	float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;

	for (uint32_t i = 0; i < n; i++) {
		if (xy[2 * i] < minx) minx = xy[2 * i];
		if (xy[2 * i] > maxx) maxx = xy[2 * i];
		if (xy[2 * i + 1] < miny) miny = xy[2 * i + 1];
		if (xy[2 * i + 1] > maxy) maxy = xy[2 * i + 1];
	}
	printf("rr(%d,%d,%ux%u,r=%u) n=%u bbox=[%.1f,%.1f .. %.1f,%.1f] "
	       "expect [%d,%d .. %d,%d]\n",
	       x, y, w, h, r, n, minx, miny, maxx, maxy,
	       x, y, x + (int)w, y + (int)h);
	printf("   v[ 0] = (%.2f, %.2f)\n", xy[0], xy[1]);
	printf("   v[%2u] = (%.2f, %.2f)\n", n - 1, xy[2 * n - 2], xy[2 * n - 1]);
}

/* Ellipse instances shared by the fill row and the ring row: (cx, rx, ry),
 * spaced so no two shapes come within 2 px of each other, vertically or
 * horizontally. */
static const struct { int32_t cx; uint32_t rx, ry; uint16_t col; } ELLS[] = {
	{ 30, 20, 20, WHITE }, { 88, 34, 14, ACC },
	{ 150, 22, 28, ACC3 }, { 206, 18, 24, ACC2 },
};
#define NELL ((int)(sizeof(ELLS) / sizeof(ELLS[0])))

#define FILL_CY 232
#define RING_CY 330

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "shapes.raw";

	T.px = fb;
	T.w = W;
	T.h = H;
	T.pitch = W;
	clear();

	/* Numeric self-checks first (return codes must all be 0), then the
	 * contour geometry, then the composed picture. */
	printf("rc fill=%d outline=%d ell_fill=%d ell_stroke=%d thin_ring=%d\n",
	       draw_rr(0, 0, 10, 10, 2, 0, WHITE),
	       draw_rr(0, 0, 10, 10, 2, 1, WHITE),
	       draw_ell(5, 5, 3, 3, 0, WHITE),
	       draw_ell(5, 5, 3, 3, 1, WHITE),
	       draw_rr(0, 0, 200, 1, 0, 1, WHITE));
	dump_rr(8, 8, 60, 44, 10);
	/* r == h/2: corner arcs touch, the flat right/left edges have zero
	 * length. This used to fail with -EINVAL (nothing drawn at all). */
	dump_rr(144, 8, 60, 44, 22);
	clear();

	/* --- row 1: rounded rect FILL, radius ramp --- */
	draw_rr(8, 8, 60, 44, 3, 0, WHITE);
	draw_rr(76, 8, 60, 44, 10, 0, ACC);
	draw_rr(144, 8, 60, 44, 22, 0, ACC3);

	/* --- row 2: the same three as 1px OUTLINE rings (the case the user
	 * reported as "rectangles abnormal") --- */
	draw_rr(8, 60, 60, 44, 3, 1, WHITE);
	draw_rr(76, 60, 60, 44, 10, 1, ACC);
	draw_rr(144, 60, 60, 44, 22, 1, ACC3);

	/* --- row 3: big radius (stadium), fill + outline --- */
	draw_rr(8, 112, 224, 40, 20, 0, ACC4);
	draw_rr(8, 160, 224, 40, 20, 1, ACC2);

	/* --- row 4: circles/ellipses as FILL --- */
	for (int i = 0; i < NELL; i++) {
		draw_ell(ELLS[i].cx, FILL_CY, ELLS[i].rx, ELLS[i].ry, 0,
			 ELLS[i].col);
	}

	/* --- row 5: the same as STROKE rings (the "circle lines missing"
	 * case) --- */
	for (int i = 0; i < NELL; i++) {
		draw_ell(ELLS[i].cx, RING_CY, ELLS[i].rx, ELLS[i].ry, 1,
			 ELLS[i].col);
	}

	FILE *f = fopen(out, "wb");

	if (!f) {
		perror(out);
		return 1;
	}
	fwrite(fb, sizeof(fb), 1, f);
	fclose(f);
	printf("wrote %s (%dx%d RGB565LE)\n", out, W, H);
	return 0;
}
