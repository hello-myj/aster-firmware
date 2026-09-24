/* SPDX-License-Identifier: MIT
 * RGB565 helpers shared by the two rasterizers that paint a gfx target: the
 * command engine (firmware/ui/ui_gfx.c) and the pretty-poly path renderer
 * (firmware/ui/pp_gfx.c). Both used to carry a private, identical copy.
 *
 * Freestanding on purpose -- <stdint.h> only, no Zephyr -- because pp_gfx.c
 * is also linked into the host shape preview (firmware/ui/hostpreview/) with
 * a plain host compiler. Keep it that way.
 *
 * Colors are plain ABI RGB565 (R in bits 15..11); the caller owns any
 * byte-order swizzle of the target words.
 */
#ifndef ASTER_CORE_COLOR565_H
#define ASTER_CORE_COLOR565_H

#include <stdint.h>

/* 565 channel (w bits) widened toward 8-bit for the lerp */
static inline uint32_t ch_up(uint32_t v, uint32_t bits)
{
	return (v << (8 - bits)) | (v >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0));
}

/* alpha = coverage in 0..255 (0 and 255 short-circuit; the integer lerp
 * below would round 255 to a neighbouring code, so the span matters). */
static inline uint16_t blend565(uint16_t d, uint16_t c, uint32_t a)
{
	if (a >= 255u) {
		return c;
	}
	if (a == 0u) {
		return d;
	}
	uint32_t dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
	uint32_t cr = (c >> 11) & 0x1F, cg = (c >> 5) & 0x3F, cb = c & 0x1F;
	uint32_t ur = ch_up(cr, 5), ug = ch_up(cg, 6), ub = ch_up(cb, 5);
	uint32_t ud = ch_up(dr, 5), ug2 = ch_up(dg, 6), ub2 = ch_up(db, 5);

	uint32_t rr = (ud * (255u - a) + ur * a) / 255u;
	uint32_t gg = (ug2 * (255u - a) + ug * a) / 255u;
	uint32_t bb = (ub2 * (255u - a) + ub * a) / 255u;

	rr = (rr + 4u) >> 3;
	gg = (gg + 2u) >> 2;
	bb = (bb + 4u) >> 3;
	if (rr > 31u) {
		rr = 31u;
	}
	if (gg > 63u) {
		gg = 63u;
	}
	if (bb > 31u) {
		bb = 31u;
	}
	return (uint16_t)((rr << 11) | (gg << 5) | bb);
}

#endif /* ASTER_CORE_COLOR565_H */
