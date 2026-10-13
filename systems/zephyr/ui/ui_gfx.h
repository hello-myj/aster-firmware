/* SPDX-License-Identifier: MIT
 * ASTER gfx command engine (ABI 2.2), firmware side.
 *
 * Rasterizes app draw commands straight into an app-owned RGB565 surface
 * buffer (the same memory aster_gfx.h inline painting uses), then reports
 * the union dirty rect for the caller to push. All entry points run on
 * the app worker thread (== UI thread); never call from another context.
 */
#ifndef ASTER_UI_GFX_H
#define ASTER_UI_GFX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <aster/gfx.h>
#include "aster_dirty.h"

/* Draw n commands into buf565 (packed rows via pitch px). Optional dirty
 * rect output (may be NULL). Returns the number of commands executed.
 * out_be selects the target's byte order: false for an app-visible ABI
 * RGB565 surface, true for a panel-order canvas (see
 * aster_plat_display_canvas_back) so the swizzle happens on the store and
 * presenting needs no conversion pass. */
/* Limit the next ordinary-surface raster to this box. w or h of 0 clears
 * it. A panel canvas ignores the box and keeps repainting the whole frame. */
void aster_gfx_set_clip(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

int aster_gfx_exec_into(uint8_t *buf565, uint32_t w, uint32_t h, uint32_t pitch,
		      const aster_gfx_cmd_t *cmds, uint32_t n,
		      const uint8_t *arena, uint32_t arena_len,
		      uint32_t *dirty_x, uint32_t *dirty_y, uint32_t *dirty_w,
		      uint32_t *dirty_h, bool out_be);

/* Measure a UTF-8 run at snapped size px (pixels). */
int32_t aster_gfx_text_measure(uint8_t px, const char *utf8, uint32_t len);

/* Decode real_path (already sandbox-resolved) into a sprite-cache slot.
 * Handle is slot index + 1 (1..6). */
int32_t aster_gfx_bmp_load_into(const char *real_path, uint32_t cap_w, uint32_t cap_h,
			      uint32_t *out_handle, uint32_t *out_w, uint32_t *out_h);
int aster_gfx_bmp_release(uint32_t handle);

/* App teardown: drop every sprite slot (shared across a single-app V1, so
 * no per-owner tracking). */
void aster_gfx_drop_all(void);

/* Last surface that a gfx frame was rasterized into, for `aster ui
 * screenshot`: game apps bypass the LVGL screen, so the LVGL snapshot is
 * empty for them. Returns false when no gfx frame has been drawn (or after
 * drop_all). All worker-thread context like the rasterizer itself.
 * *out_be reports the byte order of the returned buffer: true when the
 * frame was drawn into a panel-order canvas (see
 * ASTER_SURFACE_F_PANEL_CANVAS), in which case a consumer that wants ABI
 * RGB565 -- the screenshot encoder, mainly -- must swap each 16-bit word. */
bool aster_gfx_last_frame(const uint8_t **buf, uint32_t *w, uint32_t *h,
			uint32_t *pitch, bool *out_be);

#if IS_ENABLED(CONFIG_ASTER_GFX_TIMING)
/* Hand over the 64-frame timing numbers (aggregate raster/flush plus the
 * per-op histogram accumulated since the last call) for printing from a
 * worker thread — a log line written on the render thread costs it
 * milliseconds. Consumed by aster_native.c's "gfx tim:" cadence; the app
 * cannot measure this itself. Called from the render thread. */
void aster_gfx_timing_report(uint32_t raster_us, uint32_t flush_us,
			     uint32_t raster_max_us, uint32_t flush_max_us,
			     uint32_t cmds, uint32_t px);
#endif

/* Worker-local damage of the last successful raster batch. Copy before the next call. */
void aster_gfx_last_dirty(aster_dirty_set_t *out);

int aster_gfx_exec_unpublished_into(uint8_t *pixels,uint32_t w,uint32_t h,uint32_t pitch,
 const aster_gfx_cmd_t *cmds,uint32_t n,const uint8_t *arena,uint32_t arena_len,
 uint32_t *dx,uint32_t *dy,uint32_t *dw,uint32_t *dh);
void aster_gfx_note_committed_frame(const uint8_t *pixels,uint32_t w,uint32_t h,uint32_t pitch,bool be);
#endif /* ASTER_UI_GFX_H */
