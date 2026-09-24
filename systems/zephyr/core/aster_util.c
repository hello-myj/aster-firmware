/* SPDX-License-Identifier: MIT */
#include "aster_util.h"
#include <aster/error.h>
#include <string.h>

/* One converter per row of ASTER_FORMAT_TABLE, in the same order. Each converts
 * a single row of `w` pixels whose source and destination pointers the caller
 * has already advanced, which is why no pitch appears here.
 *
 * They are separate functions rather than inline case bodies so that the
 * dispatch below can be generated from the table; every arm is still a direct
 * call to a static function, so the compiler inlines them exactly as it did
 * the hand-written bodies. The point of the split is that a format can no
 * longer be converted-but-unadvertised (or advertised-but-unconvertible):
 * both facts now come from the same five rows. */

static void aster_row_rgb565(const uint8_t *sr, uint16_t *dr, uint32_t w)
{
	memcpy(dr, sr, w * 2);
}

static void aster_row_bgr565(const uint8_t *sr, uint16_t *dr, uint32_t w)
{
	for (uint32_t x = 0; x < w; x++) {
		uint16_t v = ((const uint16_t *)sr)[x];

		dr[x] = (uint16_t)((v >> 8) | (v << 8));
	}
}

static void aster_row_rgb888(const uint8_t *sr, uint16_t *dr, uint32_t w)
{
	for (uint32_t x = 0; x < w; x++) {
		const uint8_t *p = sr + x * 3;

		dr[x] = aster_rgb888_to_565(p[0], p[1], p[2]);
	}
}

static void aster_row_rgba8888(const uint8_t *sr, uint16_t *dr, uint32_t w)
{
	for (uint32_t x = 0; x < w; x++) {
		const uint8_t *p = sr + x * 4;

		dr[x] = aster_rgb888_to_565(p[0], p[1], p[2]);
	}
}

static void aster_row_bgra8888(const uint8_t *sr, uint16_t *dr, uint32_t w)
{
	for (uint32_t x = 0; x < w; x++) {
		const uint8_t *p = sr + x * 4;

		dr[x] = aster_rgb888_to_565(p[2], p[1], p[0]);
	}
}

int aster_convert_to_rgb565(const uint8_t *src, uint32_t src_pitch, uint32_t fmt,
			  uint16_t *dst, uint32_t dst_pitch, uint32_t w, uint32_t h)
{
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *sr = src + (size_t)y * src_pitch;
		uint16_t *dr = dst + (dst_pitch / 2) * y;

		switch (fmt) {
#define ASTER_FORMAT_CASE(fmt_, bpp_, fn_)	\
		case fmt_:			\
			fn_(sr, dr, w);		\
			break;
		ASTER_FORMAT_TABLE(ASTER_FORMAT_CASE)
#undef ASTER_FORMAT_CASE
		default:
			return ASTER_ENOTSUP;
		}
	}
	return ASTER_OK;
}
