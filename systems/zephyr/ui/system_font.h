/* SPDX-License-Identifier: MIT */
#ifndef ASTER_SYSTEM_FONT_H
#define ASTER_SYSTEM_FONT_H
#include <lvgl.h>
/* Shared offline font for system setup/settings/recovery. SD fonts remain
 * preferred when ready; this is not a second startup-only font family. */
const lv_font_t *aster_system_font(uint8_t px);
#endif
