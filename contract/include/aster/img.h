/* SPDX-License-Identifier: MIT
 * ASTER ABI 1.14 - image decoding (picture preview) surface.
 *
 * Firmware owns the decoders (BMP / JPEG / PNG) and streams them off the
 * sandboxed file system, so a picture never has to fit the app's linear
 * memory: decode straight into the app's surface back-buffer and push the
 * result with aster_surface_flush(). Decoding is constant-memory in the
 * firmware regardless of file or source-raster size (a 4K photo previews in
 * a few hundred KB of PSRAM).
 *
 * Both calls BLOCK the app worker for the decode duration (a large PNG is
 * seconds on this class of SoC), so drive them from an event (tap / swipe),
 * not from every frame.
 */
#ifndef ASTER_IMG_H
#define ASTER_IMG_H

#include "types.h"

/* Decoded format (mirrors the firmware's aster_img_fmt_t) */
#define ASTER_IMG_FMT_UNKNOWN 0u
#define ASTER_IMG_FMT_BMP     1u
#define ASTER_IMG_FMT_JPG     2u
#define ASTER_IMG_FMT_PNG     3u

/* aster_img_info_t.flags */
#define ASTER_IMG_F_UNSUPPORTED (1u << 0) /* format or variant this build cannot decode */
#define ASTER_IMG_F_TOO_BIG     (1u << 1) /* decoder would refuse the file */

/* render flags */
#define ASTER_IMG_F_NEAREST (1u << 0) /* point sampling instead of bilinear */

typedef struct {
	uint32_t size;       /* sizeof(aster_img_info_t) */
	uint32_t version;    /* 1 */
	uint32_t fmt;        /* ASTER_IMG_FMT_x (valid even when unsupported) */
	uint32_t src_w;      /* source raster, px (0 = header unreadable) */
	uint32_t src_h;
	uint32_t file_size;  /* bytes */
	uint32_t out_w;      /* what the fit-into-cap render would produce */
	uint32_t out_h;
	uint32_t flags;      /* ASTER_IMG_F_x */
	uint32_t reserved;
	char     why[48];    /* NUL-terminated note when unsupported/too_big */
} aster_img_info_t;

/* Render argument struct. Offsets are wasm linear-memory offsets. */
typedef struct {
	uint32_t size;     /* sizeof(aster_img_render_args_t) */
	uint32_t version;  /* 1 */
	uint32_t dst_off;  /* RGB565 destination, packed rows (pitch == out_w) */
	uint32_t dst_span; /* bytes reserved at dst_off (bounds check) */
	uint32_t cap_w;    /* fit-into box (surface width is the natural cap) */
	uint32_t cap_h;
	uint32_t out_w;    /* out: raster actually written */
	uint32_t out_h;    /* out: */
	uint32_t flags;    /* ASTER_IMG_F_x */
	uint32_t reserved;
} aster_img_render_args_t;

/* Identify format + dimensions WITHOUT decoding (header read only).
 * out_w/out_h carry the firmware's fit estimate against the panel (what a
 * render at full display size would produce), so the app can size its
 * buffer before committing. path is sandboxed exactly like aster_fs_open, and
 * the manifest must declare permission "fs". Returns ASTER_OK, or
 * ASTER_ENOENT / ASTER_EINVAL / ASTER_ECORRUPT / ASTER_EPERM. */
ASTER_I("aster_img_probe") int32_t aster_img_probe(const char *path, uint32_t path_len,
					     aster_img_info_t *out, uint32_t out_size);

/* Decode `path` (sandboxed; permission "fs") into args->dst_off as
 * small-endian RGB565, downscaled to fit cap_w x cap_h (aspect kept, never
 * upscaled). args->dst_span is hard-validated against the app's linear
 * memory, so an app can only scribble inside its own buffers. ASTER_OK on
 * success with out_w/out_h set; the caller then pushes the region with
 * aster_surface_flush(). */
ASTER_I("aster_img_render") int32_t aster_img_render(const char *path, uint32_t path_len,
					       aster_img_render_args_t *args,
					       uint32_t args_size);

#endif /* ASTER_IMG_H */
