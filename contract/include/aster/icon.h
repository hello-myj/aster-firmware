/* SPDX-License-Identifier: MIT
 * MJXI icon blob (16 B header + packed pixel payload), shared definition.
 *
 * Not an app-facing import: this is the container the firmware reads for
 * launcher tiles, kept here (with the rest of the ABI) because producer
 * (sdk/tools/icon_convert.py) and consumers must agree byte for byte.
 *
 * MJXI is the on-disk pixel format of every icon that reaches the panel: app
 * drawer tiles (/SD:/aster/apps/<id>/icon.bin, extracted out of the .mja icon
 * section) and the built-in launcher's chrome icons (rodata, baked by
 * sdk/tools/launcher_font_gen.py). Both are produced by
 * sdk/tools/icon_convert.py; this header is the single source of truth for
 * the layout, so a reader never re-derives the field offsets by hand.
 *
 * Layout, little-endian on disk:
 *
 *   off  sz  field
 *    0    4  magic "MJXI" (4 raw bytes)
 *    4    2  version (ASTER_ICON_VERSION)
 *    6    2  width in pixels
 *    8    2  height in pixels
 *   10    1  pixel format, ASTER_ICON_CF_x
 *   11    1  reserved (written 0; readers ignore it)
 *   12    4  payload byte count = exactly the bytes that follow the header
 *
 * Payload, rows packed one after the other with no stride padding:
 *   I1        ((w + 7) / 8) * h, MSB first
 *   A8        w * h
 *   RGB565    w * h * 2, u16 little-endian
 *   RGB565A8  w * h * 3: a full RGB565 plane, then a full A8 plane
 * Bytes past the payload are ignored (the .mja section is padded to 4).
 *
 * Do NOT overlay a packed struct on the blob, and do NOT compare the magic
 * as a u32: the blob is a little-endian byte stream, so a big-endian host
 * (or a compiler that pads the struct) would read it differently. All
 * readers go through aster_icon_parse(), which reads field by field.
 */
#ifndef ASTER_ICON_H
#define ASTER_ICON_H

#include <stdint.h>
#include <stddef.h>

#include "error.h"

/* Header */
#define ASTER_ICON_MAGIC        "MJXI"
#define ASTER_ICON_MAGIC_LEN    4u
#define ASTER_ICON_HDR_SIZE     16u
#define ASTER_ICON_VERSION      1u

/* Header field offsets (kept next to the spec above; the parser uses them so
 * the two can never drift apart). */
#define ASTER_ICON_OFF_MAGIC     0u
#define ASTER_ICON_OFF_VERSION   4u
#define ASTER_ICON_OFF_W         6u
#define ASTER_ICON_OFF_H         8u
#define ASTER_ICON_OFF_CF       10u
#define ASTER_ICON_OFF_RESERVED 11u
#define ASTER_ICON_OFF_DATA_SIZE 12u

/* Geometry ceiling: the encoder (sdk/tools/icon_convert.py) refuses anything
 * over 512x512, and a reader must not size a buffer or walk a pointer from a
 * corrupt header. Consumers may cap tighter (the launcher's drawer tiles are
 * capped at 128x128). */
#define ASTER_ICON_MAX_DIM 512u

/* Pixel format (header offset 10).
 *
 * Drawer tiles want RGB565A8: full-colour art with its own rounded corners,
 * the shape the launcher blits untouched. A8 is the original line-art mask
 * and still reads -- the launcher tints it, because a mask has no colour of
 * its own -- but it cannot carry an app's identity past a theme change.
 * sdk/tools/app_icons.py is what now produces the drawer's icons. */
#define ASTER_ICON_CF_I1       1u /* 1bpp mask, MSB first */
#define ASTER_ICON_CF_A8       2u /* 8bpp coverage; tinted at runtime */
#define ASTER_ICON_CF_RGB565   3u /* u16 little-endian */
#define ASTER_ICON_CF_RGB565A8 4u /* RGB565 plane then A8 plane */

typedef struct {
	uint16_t version;   /* ASTER_ICON_VERSION */
	uint16_t w;         /* px, 1..ASTER_ICON_MAX_DIM */
	uint16_t h;         /* px, 1..ASTER_ICON_MAX_DIM */
	uint8_t  cf;        /* ASTER_ICON_CF_x */
	uint8_t  reserved;  /* hdr[11]; 0 from all known producers */
	uint32_t data_size; /* payload bytes == aster_icon_payload_size(cf, w, h) */
} aster_icon_hdr_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Little-endian field loads (the blob is LE regardless of the host). */
static inline uint16_t aster_icon_le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t aster_icon_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Bytes the payload must occupy for a given format, 0 when cf is unknown.
 * Row-aligned (no stride padding); the value the header's data_size has to
 * match. Computed in 64 bit so even a caller that skipped the geometry check
 * cannot wrap the multiply. */
static inline uint32_t aster_icon_payload_size(uint8_t cf, uint16_t w, uint16_t h)
{
	uint64_t px = (uint64_t)w * (uint64_t)h;
	uint64_t n;

	switch (cf) {
	case ASTER_ICON_CF_I1:
		n = ((uint64_t)w + 7u) / 8u * (uint64_t)h;
		break;
	case ASTER_ICON_CF_A8:
		n = px;
		break;
	case ASTER_ICON_CF_RGB565:
		n = px * 2u;
		break;
	case ASTER_ICON_CF_RGB565A8:
		n = px * 3u;
		break;
	default:
		return 0u;
	}
	return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

/* Parse + validate one MJXI blob. `buf` is the blob as stored (the whole
 * .mja icon section, the contents of icon.bin, or a rodata icon array) and
 * `len` the bytes available at it; bytes past header+payload are allowed.
 * On ASTER_OK *out (when non-NULL) receives the host-order fields and the
 * payload starts at buf + ASTER_ICON_HDR_SIZE.
 *
 * Checks: magic, version, non-zero in-range geometry, known pixel format,
 * and data_size == the format's payload size, with header+payload inside
 * `len` -- a blob that fails any of them is ASTER_ECORRUPT (-14) and must be
 * skipped, never rendered from. A NULL buf is ASTER_EINVAL (-1). */
static inline int aster_icon_parse(const uint8_t *buf, size_t len,
				 aster_icon_hdr_t *out)
{
	aster_icon_hdr_t h;
	uint32_t need;

	if (!buf) {
		return ASTER_EINVAL;
	}
	if (len < ASTER_ICON_HDR_SIZE) {
		return ASTER_ECORRUPT;	/* truncated before the header ends */
	}
	/* Byte-wise magic: a u32 compare would only hold on a little-endian
	 * host (see the note at the top). */
	for (uint32_t i = 0; i < ASTER_ICON_MAGIC_LEN; i++) {
		if (buf[ASTER_ICON_OFF_MAGIC + i] != (uint8_t)ASTER_ICON_MAGIC[i]) {
			return ASTER_ECORRUPT;
		}
	}
	h.version = aster_icon_le16(buf + ASTER_ICON_OFF_VERSION);
	h.w = aster_icon_le16(buf + ASTER_ICON_OFF_W);
	h.h = aster_icon_le16(buf + ASTER_ICON_OFF_H);
	h.cf = buf[ASTER_ICON_OFF_CF];
	h.reserved = buf[ASTER_ICON_OFF_RESERVED];
	h.data_size = aster_icon_le32(buf + ASTER_ICON_OFF_DATA_SIZE);

	if (h.version != ASTER_ICON_VERSION ||
	    h.w == 0u || h.h == 0u ||
	    h.w > ASTER_ICON_MAX_DIM || h.h > ASTER_ICON_MAX_DIM) {
		return ASTER_ECORRUPT;
	}
	need = aster_icon_payload_size(h.cf, h.w, h.h);
	if (need == 0u || h.data_size != need ||
	    (size_t)ASTER_ICON_HDR_SIZE + h.data_size > len) {
		return ASTER_ECORRUPT;
	}
	if (out) {
		*out = h;
	}
	return ASTER_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* ASTER_ICON_H */
