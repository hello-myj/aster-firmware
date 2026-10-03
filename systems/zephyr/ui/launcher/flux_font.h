/* SPDX-License-Identifier: MIT */
#ifndef ASTER_FLUX_FONT_H
#define ASTER_FLUX_FONT_H
#include <lvgl.h>
unsigned aster_flux_font_pixels(unsigned size,unsigned zoom,const char *text);
const lv_font_t *aster_flux_font(unsigned pixels,const char *text);
#endif
