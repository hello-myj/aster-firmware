/* SPDX-License-Identifier: MIT
 * Image decoder front-end (design: picture viewer).
 *
 * Pure-CPU decode of SD-resident bitmap files into small-endian RGB565,
 * downscaled to fit the panel (preview). All three formats stream: memory
 * stays decoupled from file AND raster size, so a 4K source (tens of MB
 * BMP, or a huge PNG) previews in a constant few hundred KB of PSRAM.
 *
 * Threading: all calls block the caller until done; run them from the
 * shell or a dedicated thread, never a ISR. Large buffers are allocated
 * from PSRAM internally; the caller supplies only the RGB565 output.
 */
#ifndef ASTER_IMGDEC_H
#define ASTER_IMGDEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <aster/error.h>

typedef enum {
	ASTER_IMG_UNKNOWN = 0,
	ASTER_IMG_BMP,
	ASTER_IMG_JPG,
	ASTER_IMG_PNG,
} aster_imgdec_fmt_t;

typedef struct {
	aster_imgdec_fmt_t fmt;
	uint32_t src_w;      /* pixels */
	uint32_t src_h;      /* pixels (abs for BMP bottom-up) */
	uint32_t file_size;  /* bytes, from fs_stat */
	uint32_t out_w;      /* fit-into-cap preview size (0 if unknown) */
	uint32_t out_h;
	bool     too_big;    /* decoder would refuse (over PSRAM / not streamed) */
	bool     unsupported;/* format or variant not decodable in this build */
	char     why[48];    /* human note for too_big/unsupported */
} aster_imgdec_info_t;

/* Identify format + dimensions WITHOUT decoding. Reads only headers.
 * cap_w/cap_h drive the fit estimate in info->out_w/out_h.
 * Returns ASTER_OK or a negative aster_error_t. */
aster_error_t aster_imgdec_probe(const char *path, aster_imgdec_info_t *info,
			  uint32_t cap_w, uint32_t cap_h);

/* Decode path + downscale (fit into cap_w x cap_h, aspect kept) into
 * out as small-endian RGB565. Row pitch of the output is *out_w pixels
 * (out must be at least cap_w*cap_h*2 bytes). Sets *out_w and *out_h.
 * nearest=true uses point sampling (BMP reads 1 source row per output
 * row -> faster on huge images). No display side effects. ASTER_OK on success. */
aster_error_t aster_imgdec_render(const char *path, uint8_t *out,
			   uint32_t cap_w, uint32_t cap_h,
			   uint32_t *out_w, uint32_t *out_h, bool nearest);

/* Convenience for the CLI: probe + render into a PSRAM canvas, then blit
 * centered on the display (black backdrop). Blocks until drawn. */
aster_error_t aster_imgdec_show(const char *path, bool nearest);

/* Format name for logs ("BMP"/"JPG"/"PNG"/"?"). */
const char *aster_imgdec_fmt_name(aster_imgdec_fmt_t fmt);

#endif /* ASTER_IMGDEC_H */
