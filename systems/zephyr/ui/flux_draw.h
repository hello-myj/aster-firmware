/* SPDX-License-Identifier: MIT */
#ifndef ASTER_FLUX_DRAW_H
#define ASTER_FLUX_DRAW_H
#include <stdint.h>
/* Primitive types keep this usable from lv_conf.h, before LVGL types exist. */
int aster_flux_draw_image(int transformed, int format, const void *src,
                         const void *coords, uint32_t stride,
                         const void *clip, void *task, const void *descriptor);
uint32_t aster_flux_draw_time_reset(void);
void aster_flux_draw_prepare(const void *source,uint32_t stride);
void aster_flux_draw_forget(const void *source);
#endif
