/* SPDX-License-Identifier: MIT
 * imgdec internal: helpers shared between the front-end (imgdec.c) and the
 * per-format decoders (imgdec_jpg.c, ...). Not a public header.
 */
#ifndef ASTER_IMGDEC_INTERNAL_H
#define ASTER_IMGDEC_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "imgdec.h"

static inline int img_clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* Largest scale (as float, <=1.0) so src fits the cap, aspect kept. */
void img_fit_size(uint32_t sw, uint32_t sh, uint32_t cw, uint32_t ch,
		  uint32_t *ow, uint32_t *oh, double *scale);

/* JPEG (TJpgDec). probe parses SOF for dimensions; render loads the whole
 * file into PSRAM, decodes at the coarsest DCT scale that still covers the
 * fit target, then downsamples to out. */
#if IS_ENABLED(CONFIG_ASTER_IMGDEC_JPG)
aster_error_t aster_jpg_probe(const char *path, aster_imgdec_info_t *info,
			  uint32_t cap_w, uint32_t cap_h);
aster_error_t aster_jpg_render(const char *path, uint8_t *out,
			   uint32_t cap_w, uint32_t cap_h,
			   uint32_t *out_w, uint32_t *out_h, bool nearest);
#else
static inline aster_error_t aster_jpg_probe(const char *path, aster_imgdec_info_t *info,
					uint32_t cap_w, uint32_t cap_h)
{
	ARG_UNUSED(path); ARG_UNUSED(cap_w); ARG_UNUSED(cap_h);
	info->fmt = ASTER_IMG_JPG;
	info->unsupported = true;
	snprintf(info->why, sizeof(info->why), "JPG disabled in build");
	return ASTER_OK;
}
static inline aster_error_t aster_jpg_render(const char *path, uint8_t *out,
					 uint32_t cap_w, uint32_t cap_h,
					 uint32_t *ow, uint32_t *oh, bool nn)
{
	ARG_UNUSED(path); ARG_UNUSED(out); ARG_UNUSED(cap_w); ARG_UNUSED(cap_h);
	ARG_UNUSED(ow); ARG_UNUSED(oh); ARG_UNUSED(nn);
	return ASTER_ENOTSUP;
}
#endif

/* PNG (miniz tinfl). probe reads IHDR; render streams IDAT inflate ->
 * unfilter -> RGB565 row cache -> bilinear emit, memory decoupled from
 * both file and raster size. */
#if IS_ENABLED(CONFIG_ASTER_IMGDEC_PNG)
aster_error_t aster_png_probe(const char *path, aster_imgdec_info_t *info,
			  uint32_t cap_w, uint32_t cap_h);
aster_error_t aster_png_render(const char *path, uint8_t *out,
			   uint32_t cap_w, uint32_t cap_h,
			   uint32_t *out_w, uint32_t *out_h, bool nearest);
#else
static inline aster_error_t aster_png_probe(const char *path, aster_imgdec_info_t *info,
					uint32_t cap_w, uint32_t cap_h)
{
	ARG_UNUSED(path); ARG_UNUSED(cap_w); ARG_UNUSED(cap_h);
	info->fmt = ASTER_IMG_PNG;
	info->unsupported = true;
	snprintf(info->why, sizeof(info->why), "PNG disabled in build");
	return ASTER_OK;
}
static inline aster_error_t aster_png_render(const char *path, uint8_t *out,
					 uint32_t cap_w, uint32_t cap_h,
					 uint32_t *ow, uint32_t *oh, bool nn)
{
	ARG_UNUSED(path); ARG_UNUSED(out); ARG_UNUSED(cap_w); ARG_UNUSED(cap_h);
	ARG_UNUSED(ow); ARG_UNUSED(oh); ARG_UNUSED(nn);
	return ASTER_ENOTSUP;
}
#endif

#endif /* ASTER_IMGDEC_INTERNAL_H */
