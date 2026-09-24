/* SPDX-License-Identifier: MIT
 * Endianness helpers shared by every binary parser (MJA packages, .mja
 * signatures, imgdec BMP/PNG/JPEG, ID3 tags/lyrics).
 *
 * freestanding on purpose: pp_gfx.c compiles under a host compiler for the
 * preview tool (firmware/ui/hostpreview/preview.c) with no Zephyr headers
 * around, so this file must only ever include <stdint.h>.
 *
 * Every accessor assembles the word byte by byte. Do NOT "optimise" these
 * into a cast-and-deref: callers hand us unaligned pointers into file
 * buffers (BMP/PNG/ID3 fields at odd offsets), which such a load would
 * fault on or silently assume aligned.
 */
#ifndef ASTER_BYTES_H
#define ASTER_BYTES_H

#include <stdint.h>

/* p[0] | p[1]<<8 */
static inline uint16_t aster_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

/* p[0]<<8 | p[1] */
static inline uint16_t aster_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

/* byte-wise little endian */
static inline uint32_t aster_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* byte-wise big endian */
static inline uint32_t aster_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* little-endian signed 16 */
static inline int16_t aster_ld16s(const uint8_t *p)
{
	return (int16_t)(p[0] | (p[1] << 8));
}

/* pure byte swap: (v<<8)|(v>>8) */
static inline uint16_t aster_bswap16(uint16_t v)
{
	return (uint16_t)((v << 8) | (v >> 8));
}

/* ID3v2 synchsafe 28-bit size: four big-endian 7-bit groups. */
static inline uint32_t aster_synchsafe32(const uint8_t *p)
{
	return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
	       ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

#endif /* ASTER_BYTES_H */
