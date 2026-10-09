/* SPDX-License-Identifier: MIT */
#ifndef ASTER_SURFACE_H
#define ASTER_SURFACE_H

#include "types.h"

/* aster_surface_desc_t.flags */
/* Ask for a panel-order streaming canvas instead of an app-visible buffer.
 * The firmware then draws the gfx engine's output into its own full-screen
 * front buffer, already in the byte order the display latches, so a frame
 * costs one cache flush and a hand-off to the DMA worker rather than a
 * 307 KB conversion sweep (~7 ms at the measured 33 MB/s PSRAM store
 * bandwidth). acquire() reports ptr == NULL for such a surface -- there is
 * no app-side pixel memory and nothing to read back.
 *
 * This is a trade, and the app has to hold up its end: the two front
 * buffers alternate, so the one being drawn into holds no previous frame.
 * The app MUST repaint every pixel on every present (a full-screen clear
 * plus the scene, i.e. aster_ge_present must see a whole-canvas dirty rect);
 * partial repaints would leave stale pixels in the untouched area. Apps
 * with anything else to draw -- screenshots, readback, partial updates --
 * should leave the flag clear and keep the ordinary surface.
 * Firmware may ignore the flag (rotated panel, buffers unavailable); check
 * flags in aster_surface_buffer_t to see what you got. */
#define ASTER_SURFACE_F_PANEL_CANVAS 0x00000001u
/* Opt-in compatibility canvas. The app supplies an explicit width and height;
 * that buffer is its coordinate space. On present the firmware scales it onto
 * the glass with keep-aspect (scale = min(panel/buf) on each axis), centers
 * the result and paints the unused margins black. Scale may be greater or
 * less than one. Touch samples that belong to the surface are mapped back by
 * subtracting the margin and dividing by the same scale, then clamped into
 * the buffer. Do not combine with ASTER_SURFACE_F_PANEL_CANVAS: that would
 * scale a buffer that is already the panel. Surfaces that leave this clear
 * stay 1:1 with the screen. */
#define ASTER_SURFACE_F_SCALE_FIT 0x00000002u

typedef struct {
	uint32_t size;    /* sizeof(aster_surface_desc_t) */
	uint32_t version; /* 1 */
	uint32_t width;   /* 0 = display width */
	uint32_t height;  /* 0 = display height */
	uint32_t format;  /* aster_pixel_format_t; ASTER_PIXEL_NATIVE allowed */
	uint32_t flags;
} aster_surface_desc_t;

/* Snapshot of the surface back-buffer. ptr is a wasm linear-memory pointer
 * the app may write into until aster_surface_present() is called -- unless
 * flags reports ASTER_SURFACE_F_PANEL_CANVAS, in which case ptr is NULL and
 * the pixels live in firmware-owned memory the app can only reach through
 * the gfx engine. */
typedef struct {
	uint32_t size;    /* sizeof(aster_surface_buffer_t) */
	uint32_t version; /* 1 */
	void     *ptr;    /* only valid in the app's linear memory */
	uint32_t width;
	uint32_t height;
	uint32_t pitch;   /* bytes per row */
	uint32_t format;  /* actual buffer format (resolved NATIVE) */
	uint32_t flags;   /* ASTER_SURFACE_F_*; what the firmware granted */
} aster_surface_buffer_t;

ASTER_I("aster_surface_create") int32_t aster_surface_create(const aster_surface_desc_t *desc,
						       uint32_t desc_size, aster_handle_t *out);
ASTER_I("aster_surface_destroy") int32_t aster_surface_destroy(aster_handle_t surface);
ASTER_I("aster_surface_acquire") int32_t aster_surface_acquire(aster_handle_t surface,
							 aster_surface_buffer_t *buf, uint32_t buf_size);
/* Copy the current buffer content to the display (format conversion if needed). */
ASTER_I("aster_surface_present") int32_t aster_surface_present(aster_handle_t surface);

/* ABI 1.4: push a rectangular dirty region straight from `src` (app linear
 * memory, RGB565, row pitch src_pitch pixels) to the display at (x,y). The
 * surface handle only carries format/bounds checks -- src need NOT be the
 * surface buffer, which is what lets a partial-rendering UI toolkit stream
 * its draw buffers without a full-frame staging copy. Requires surface
 * format ASTER_PIXEL_RGB565; region must fit the surface. Apps importing this
 * MUST declare abi_min >= 1.4 (older firmware rejects at install). */
ASTER_I("aster_surface_flush") int32_t aster_surface_flush(aster_handle_t surface, const void *src,
						    uint32_t x, uint32_t y, uint32_t w,
						    uint32_t h, uint32_t src_pitch);

#endif /* ASTER_SURFACE_H */
