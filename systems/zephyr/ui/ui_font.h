/* SPDX-License-Identifier: MIT
 * LVGL font router: every glyph comes from a pre-baked cpfont face on the
 * SD card (firmware/ui/font_sd.c), streamed by size. The old MJXF paths
 * (pack subsets, SD reader bins, rodata bakes, dynamic sysfont subsets) and
 * the lv_tiny_ttf pool that replaced them are all gone.
 *
 * No face available -> placeholder font (invisible text, never crashes);
 * see the LOG_ERR in ui_font.c, which is the only thing that says so.
 */
#ifndef ASTER_UI_FONT_H
#define ASTER_UI_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lvgl.h>

/* LVGL image-asset budget (MJXI app icons; font metrics live in dedicated
 * PSRAM allocations in font_sd.c, not here). cpfont files reach ~5 MB. */
#define ASTER_UI_ASSET_BUDGET   (256u * 1024u)
#define ASTER_UI_ASSET_FILE_MAX (32u * 1024u * 1024u)

/* Baked font for `px`, snapping to the nearest baked size within a few
 * pixels. `px == 0` means "whatever the UI uses" (ASTER_FONT_UI_PX). Never
 * NULL: falls back to aster_ui_placeholder_font outside the baked set. */
const lv_font_t *aster_ui_font(uint8_t px);

/* Advances the cpfont state machine and commits bitmaps that landed
 * asynchronously; returns 1 when the caller should repaint. UI thread.
 *
 * Retires the guest-facing repaint request on the way out, so this is the
 * launcher's entry point: there is no guest to answer. */
int aster_ui_fonts_poll(void);

/* Same two stages WITHOUT retiring the repaint request -- the app-side entry
 * point, called from ui_service() while a guest owns the worker.
 *
 * The split is load-bearing. aster_font_sd_repaint_ack() clears the level
 * signal that ABI 3.1's aster_gfx_fonts_pending() answers from, so acking
 * here would swallow the guest's own notification that glyphs arrived and
 * it would never repaint them. Returns 1 when new bitmaps landed (caller
 * invalidates the sheet); leave the ack to aster_ui_fonts_poll. UI thread. */
int aster_ui_fonts_advance(void);

/* Queue `utf8`'s codepoints for async bitmap prefetch at the snapped
 * cpfont size (no-op when px is outside the baked set). UI thread. */
void aster_ui_font_warm(uint8_t px, const char *utf8);

#endif /* ASTER_UI_FONT_H */
