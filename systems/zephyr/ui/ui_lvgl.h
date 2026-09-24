/* SPDX-License-Identifier: MIT */
#ifndef ASTER_UI_LVGL_H
#define ASTER_UI_LVGL_H

#include <stdint.h>
#include <lvgl.h>

int  aster_ui_lvgl_init(void);
void aster_ui_lvgl_pointer(uint32_t x, uint32_t y, int pressed);
void aster_ui_lvgl_pump(void);
lv_obj_t *aster_ui_lvgl_screen(void);
uint32_t aster_ui_lvgl_width(void);
uint32_t aster_ui_lvgl_height(void);

/* Re-point the display at the window the panel is showing now, if it moved.
 * A no-op when it did not, so callers do not need to know whether anything
 * turned. aster_ui_lvgl_pump() calls it every pass; the one other caller is
 * aster_launcher_show(), which has to ask BEFORE it builds: the launcher puts
 * its widgets at absolute positions derived from width()/height(), and those
 * numbers are read once, at build time. */
void aster_ui_lvgl_resync_window(void);

/* The one pointer indev, so a gesture consumer can call
 * lv_indev_wait_release() on the stroke it just swallowed -- without it
 * LVGL still fires CLICKED on the widget under the finger when the
 * release lands. NULL before aster_ui_lvgl_init() has succeeded (and
 * lv_indev_wait_release() itself tolerates NULL). */
lv_indev_t *aster_ui_lvgl_indev(void);

#endif
