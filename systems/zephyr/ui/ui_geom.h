/* SPDX-License-Identifier: MIT
 * The LOGICAL screen geometry, as a compile-time fact.
 *
 * `aster_ui_lvgl_width()/height()` return these same numbers at runtime. The
 * firmware UI can do better than asking, though: the panel's window is a
 * devicetree property, so layout arithmetic that is expressed through the
 * macros below stops compiling when the panel's geometry changes, instead of
 * quietly keeping the old numbers. That distinction is the whole point --
 * every layout bug this header exists to prevent was silent, not wrong-looking
 * (a drawer grid that no longer spans the width, a gesture band that can no
 * longer be reached).
 *
 * sdk/apps cannot use this: the app layer is hardware-free and never sees the
 * devicetree (sdk/tools/check_layering.py). Apps use the runtime accessors,
 * which is why those still exist.
 */
#ifndef ASTER_UI_GEOM_H_
#define ASTER_UI_GEOM_H_

#include <zephyr/devicetree.h>

#define ASTER_DISPLAY_PANEL_W DT_PROP(DT_CHOSEN(zephyr_display), width)
#define ASTER_DISPLAY_PANEL_H DT_PROP(DT_CHOSEN(zephyr_display), height)

/* Rotation SWAPS the axes, so these cannot be aliases. It is 0 on the
 * reference board -- both of its orientations are the panel rotating itself,
 * see the board overlay -- and non-zero only for controllers that cannot,
 * where the platform transposes on the app's behalf. */
#if CONFIG_ASTER_DISPLAY_ROTATION == 90 || CONFIG_ASTER_DISPLAY_ROTATION == 270
#define ASTER_UI_W ASTER_DISPLAY_PANEL_H
#define ASTER_UI_H ASTER_DISPLAY_PANEL_W
#else
#define ASTER_UI_W ASTER_DISPLAY_PANEL_W
#define ASTER_UI_H ASTER_DISPLAY_PANEL_H
#endif

/* Which way round we are, for layouts that differ by ARRANGEMENT rather than
 * by arithmetic -- a grid gains a column where a home page gains a second
 * text column, and neither is a formula. */
#define ASTER_UI_LANDSCAPE (ASTER_UI_W > ASTER_UI_H)

#endif /* ASTER_UI_GEOM_H_ */
