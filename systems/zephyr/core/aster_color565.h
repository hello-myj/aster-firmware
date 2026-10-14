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

/* Coverage is interpolated directly in native channel space and rounded once.
 * Expanding to 8 bits then adding half a truncation step on conversion back
 * brightens equal colors (and can create halos in antialiased text).
 * Preserve endpoints, identity, symmetry and the channel convex hull. */
static inline uint16_t blend565(uint16_t d, uint16_t c, uint32_t a)
{
    if(a >= 255u) return c;
    if(a == 0u) return d;
    uint32_t inverse = 255u - a;
    uint32_t r = (((d >> 11) & 31u) * inverse + ((c >> 11) & 31u) * a + 127u) / 255u;
    uint32_t g = (((d >> 5) & 63u) * inverse + ((c >> 5) & 63u) * a + 127u) / 255u;
    uint32_t b = ((d & 31u) * inverse + (c & 31u) * a + 127u) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

#endif /* ASTER_CORE_COLOR565_H */
