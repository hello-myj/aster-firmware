#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * Host LVGL display + pointer. PARTIAL bands live in PSRAM.
 */
#include "ui_lvgl.h"
#include "font_sd.h"
#include "aster_platform.h"
#include "aster_mem.h"
#include "aster_runtime.h"
#include "qpanel.h"
#if IS_ENABLED(CONFIG_ASTER_SCREENSHOT)
#include "ui_screenshot.h"
#endif

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if IS_ENABLED(CONFIG_ASTER_PANEL_AXS15231B)
#include <zephyr/multi_heap/shared_multi_heap.h>
#endif

LOG_MODULE_REGISTER(aster_ui_lvgl, LOG_LEVEL_INF);

#ifndef CONFIG_ASTER_LVGL_BAND_ROWS
#define CONFIG_ASTER_LVGL_BAND_ROWS 80
#endif

static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static uint8_t *s_band[2];
static uint32_t s_band_bytes;
static uint32_t s_w, s_h;
static int32_t s_tx, s_ty;
static int s_tpressed;
static int s_ready;
static bool s_band_warned;	/* a rejected band is already in the log */

/* profiler (CONFIG_ASTER_LVGL_PROFILER) */
static uint32_t s_flush_px;
static uint32_t s_flush_bytes;
static uint32_t s_flush_n;
static uint64_t s_prof_ms;
static uint64_t s_frame_us, s_wait_us;
static uint64_t s_submit_us;
static uint32_t s_frame_n, s_frame_max_us;

void aster_ui_lvgl_assert(void)
{
	LOG_ERR("lvgl assert");
	k_oops();
}

static uint32_t tick_cb(void)
{
	return (uint32_t)k_uptime_get();
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	/* A guest draws directly to the panel; the empty LVGL screen is not
	 * its background. Launcher teardown and delayed font invalidations
	 * still refresh that screen while timers and screenshot polling run.
	 * Only the open control panel owns LVGL pixels over a running app. */
	if (aster_app_current() && !aster_qpanel_is_open()) {
		LOG_DBG("skip LVGL background over app: %d,%d..%d,%d",
			area->x1, area->y1, area->x2, area->y2);
		lv_display_flush_ready(disp);
		return;
	}
	uint32_t x = (uint32_t)area->x1;
	uint32_t y = (uint32_t)area->y1;
	uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
	uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
	if (IS_ENABLED(CONFIG_ASTER_LAUNCHER_FLUX)) {
		/* DIRECT redraws only damaged regions in a complete framebuffer.
		 * AXS QSPI needs a stream from row zero, once after the last region. */
		if (!lv_display_flush_is_last(disp)) {
			lv_display_flush_ready(disp);
			return;
		}
		x = y = 0;
		w = s_w;
		h = s_h;
	}
	uint32_t pitch = w;
	lv_draw_buf_t *db = lv_display_get_buf_active(disp);

	if (db && db->header.stride >= (w * 2u)) {
		pitch = db->header.stride / 2u;
	}
	/* SWAPPED draw buffers already hold panel byte order: at rot 0 the
	 * platform ships px_map straight to the SPI driver (no gather). */
	uint32_t submit_start=k_cycle_get_32();
	int err;
#if IS_ENABLED(CONFIG_ASTER_LAUNCHER_FLUX) && IS_ENABLED(CONFIG_ASTER_PANEL_AXS15231B)
 err=aster_plat_display_write_persistent_ui(px_map,w,h,pitch);
 if(err==ASTER_EINVAL)
#endif
 err = aster_plat_display_write_at_ex(px_map, x, y, w, h, pitch, true);
	s_submit_us+=k_cyc_to_us_floor64((uint32_t)(k_cycle_get_32()-submit_start));

	if (err != ASTER_OK) {
		/* The platform checks the rect against the LOGICAL screen, so a
		 * band that no longer fits is refused -- and this call used to
		 * simply drop that answer on the floor, which is how a stale
		 * LVGL resolution (see below) presents itself as a half-drawn
		 * screen with an empty log. Say it once per run of failures
		 * rather than once per band, since the condition repeats every
		 * frame until something changes. */
		if (!s_band_warned) {
			s_band_warned = true;
			LOG_WRN("band %ux%u+%u+%u rejected (%d): LVGL is drawing "
				"for a %ux%u screen the platform no longer has",
				w, h, x, y, err, s_w, s_h);
		}
	} else {
		s_band_warned = false;
	}
	s_flush_px += w * h;
	s_flush_bytes += w * h * 2u;
	s_flush_n++;
	/* Keep LVGL's frame in flight until flush_wait_cb has drained DMA.
     * Signalling ready at submission bypasses DIRECT's completion fence;
     * buffer/cache ownership must remain with the transfer until it ends. */
    if (!IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)) {
        lv_display_flush_ready(disp);
    }
}

#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
/* LVGL calls this from wait_for_flushing() (double-buffered PARTIAL mode)
 * right before it may reuse a band -- one full render AFTER flush_cb
 * posted, which is exactly the pipeline overlap window: the worker only
 * borrows the CPU per SPI chunk (higher prio, sleeps on trans_done), so
 * rendering and streaming interleave on this single core. */
static void flush_wait_cb(lv_display_t *disp)
{
	ARG_UNUSED(disp);
	uint32_t start = k_cycle_get_32();
	aster_plat_display_flush_drain();
	if (IS_ENABLED(CONFIG_ASTER_LVGL_PROFILER)) {
		s_wait_us += k_cyc_to_us_floor64((uint32_t)(k_cycle_get_32() - start));
	}
}
#endif

static void pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	ARG_UNUSED(indev);
	data->point.x = s_tx;
	data->point.y = s_ty;
	data->state = s_tpressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

int aster_ui_lvgl_init(void)
{
	aster_display_info_t di = { .size = sizeof(di), .version = 1 };

	if (s_ready) {
		return ASTER_OK;
	}
	if (aster_plat_display_get_info(0, &di) != ASTER_OK || !di.width || !di.height) {
		return ASTER_ENOENT;
	}
	s_w = di.width;
	s_h = di.height;

	lv_init();
	lv_tick_set_cb(tick_cb);
	aster_font_sd_init();

	uint32_t rows = CONFIG_ASTER_LVGL_BAND_ROWS;
	if (rows > s_h) {
		rows = s_h;
	}
	if (rows < 16) {
		rows = 16;
	}
	/* Sized for the LONG side, not the width at boot: the window can be
	 * turned under the UI (S5/S6 -- an app declares an orientation and the
	 * runtime honours it, and `hw display orient` turns it by hand too), and
	 * a turn swaps the axes. Allocating for the boot width would make the
	 * landscape build right by luck and the portrait build wrong by 80 KB:
	 * a band of max(w,h) is the same number in both orientations, so one
	 * allocation covers both and aster_ui_lvgl_resync_window() never has to
	 * reallocate in the middle of a frame. */
	const uint32_t band_w = MAX(s_w, s_h);
	/* AXS15231B QSPI streams rows sequentially. Partial LVGL rectangles
	 * otherwise force several full-shadow retransmits per refresh. FLUX
	 * uses full PSRAM buffers and sends exactly one complete frame. */
	s_band_bytes = IS_ENABLED(CONFIG_ASTER_LAUNCHER_FLUX) ?
		s_w * s_h * 2u : band_w * rows * 2u;
#if IS_ENABLED(CONFIG_ASTER_PANEL_AXS15231B)
 /* GDMA external bursts and all chained descriptors require 64 B alignment. */
 s_band[0]=shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL,64,s_band_bytes);
 s_band[1]=shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL,64,s_band_bytes);
#else
 s_band[0] = aster_psram_alloc(s_band_bytes);
 s_band[1] = aster_psram_alloc(s_band_bytes);
#endif
	if (!s_band[0] || !s_band[1]) {
		aster_psram_free(s_band[0]);
		aster_psram_free(s_band[1]);
		s_band[0] = s_band[1] = NULL;
		return ASTER_ENOMEM;
	}

	s_disp = lv_display_create((int32_t)s_w, (int32_t)s_h);
	lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
	lv_display_set_buffers(s_disp, s_band[0], s_band[1], s_band_bytes,
			       IS_ENABLED(CONFIG_ASTER_LAUNCHER_FLUX) ?
			       LV_DISPLAY_RENDER_MODE_DIRECT : LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(s_disp, flush_cb);
#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
	lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);
#endif

	s_indev = lv_indev_create();
	lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(s_indev, pointer_read_cb);

	s_ready = 1;
	LOG_INF("native lvgl %ux%u band=%u rows/%u max", s_w, s_h, rows, band_w);
	return ASTER_OK;
}

void aster_ui_lvgl_resync_window(void)
{
	aster_display_info_t di = { .size = sizeof(di), .version = 1 };
	uint32_t ow, oh;

	if (!s_ready) {
		return;
	}
	if (aster_plat_display_get_info(0, &di) != ASTER_OK || !di.width || !di.height) {
		return;
	}
	if (di.width == s_w && di.height == s_h) {
		return;
	}

	/* Nothing may be mid-flight before the buffers are re-pointed: the
	 * re-init below hands LVGL back to buf_1, and a queued direct-send job
	 * may still be reading buf_2 off the wire. */
	aster_plat_display_flush_drain();

	/* The record describes the window the panel has stopped showing, and the
	 * repaint at the bottom of this function is what fills it back in -- so
	 * it is re-armed HERE, on the thread that is about to emit those bands,
	 * and not where the panel was turned. Doing it at the turn meant blanking
	 * from another thread, which landed between two bands of this very
	 * repaint: measured on the reference board, band 1 (240x213+0+0) was
	 * composited and then wiped, band 2 (240x107+0+213) survived, and the
	 * grab showed a crop of the screen that reads as a coherent picture. */
	aster_plat_display_glass_rearm();

	ow = s_w;
	oh = s_h;
	s_w = di.width;
	s_h = di.height;
	s_band_warned = false; /* whatever was refused was drawn for the old window */

	lv_display_set_resolution(s_disp, (int32_t)s_w, (int32_t)s_h);
	/* Resolution alone is not enough: the band's STRIDE is baked into the
	 * draw buffer's header at set_buffers() time and flush_cb() hands that
	 * stride to the panel as the row pitch. Resizing without re-initing
	 * would announce a band of the new width at the OLD width's pitch --
	 * every row after the first would come off the wire sheared. The
	 * capacity is unchanged (see the long-side allocation in init), so this
	 * rebuilds the headers in place; nothing is allocated here. */
	lv_display_set_buffers(s_disp, s_band[0], s_band[1], s_band_bytes,
			       IS_ENABLED(CONFIG_ASTER_LAUNCHER_FLUX) ?
			       LV_DISPLAY_RENDER_MODE_DIRECT : LV_DISPLAY_RENDER_MODE_PARTIAL);

	/* Repaint the lot, before the pump hands LVGL back to its caller. The
	 * panel has lost nothing -- the controller is scanning the same frame
	 * memory the other way round -- but on this side everything the tree
	 * last drew belongs to the old window, and the record was just re-armed
	 * above on the strength of this very repaint happening. Measured on the
	 * reference board: without this, a hand-typed turn left the record
	 * holding a handful of stale rows -- a grab that looks like a coherent
	 * picture of the wrong thing, which is worse than an empty one. The
	 * invalidation inside lv_display_set_resolution() is not something to
	 * lean on here: it is that library's business, and this function's
	 * contract is that the UI matches the panel when it returns. */
	lv_obj_invalidate(lv_screen_active());

	/* Says out loud what the band warning used to be the only sign of: the
	 * UI stopped matching the panel and has been put back. Expected at
	 * every app that declares an orientation (S6 turns the panel for the
	 * length of its run), and at `hw display orient` typed by hand. */
	LOG_INF("lvgl window -> %ux%u (was %ux%u)", s_w, s_h, ow, oh);
}

void aster_ui_lvgl_pointer(uint32_t x, uint32_t y, int pressed)
{
	s_tx = (int32_t)x;
	s_ty = (int32_t)y;
	s_tpressed = pressed ? 1 : 0;
}

void aster_ui_lvgl_pump(void)
{
    if (!aster_plat_display_count()) return;
	if (!s_ready) {
		return;
	}
	/* Before rendering, not after: LVGL's idea of the screen has to be the
	 * window the panel is actually showing or every band it emits is drawn
	 * for a rectangle that is not there. Every path that turns the panel
	 * (the S6 lease, `hw display orient`) goes through here eventually, and
	 * this is the one place that knows it must. Costs one get_info() per
	 * pass and returns immediately when nothing moved. */
	aster_ui_lvgl_resync_window();
	uint32_t frame_start = k_cycle_get_32();
	uint32_t prior_flushes = s_flush_n;
	lv_timer_handler();
	if (IS_ENABLED(CONFIG_ASTER_LVGL_PROFILER) && s_flush_n != prior_flushes) {
		uint32_t elapsed = k_cyc_to_us_floor64((uint32_t)(k_cycle_get_32() - frame_start));
		s_frame_us += elapsed;
		s_frame_max_us = MAX(s_frame_max_us, elapsed);
		s_frame_n++;
	}
#if IS_ENABLED(CONFIG_ASTER_SCREENSHOT)
	aster_screenshot_poll(); /* shell `ui screenshot` grab, on ours */
#endif
	if (IS_ENABLED(CONFIG_ASTER_LVGL_PROFILER)) {
		uint64_t now = (uint64_t)k_uptime_get();
		if (now - s_prof_ms >= 2000 && s_flush_n) {
			aster_font_sd_stats_t fs;

			aster_font_sd_stats(&fs);
			LOG_INF("ui prof: flush=%u dirty_px=%u bytes=%u band=%u",
				s_flush_n, s_flush_px, s_flush_bytes,
				CONFIG_ASTER_LVGL_BAND_ROWS);
			if (s_frame_n) {
				LOG_INF("ui frame: n=%u avg=%u us max=%u us wait=%u us/frame",
					s_frame_n, (unsigned)(s_frame_us / s_frame_n),
					s_frame_max_us, (unsigned)(s_wait_us / s_frame_n));
				LOG_INF("ui submit: avg=%u us",(unsigned)(s_submit_us/s_frame_n));
#if IS_ENABLED(CONFIG_ASTER_LAUNCHER_FLUX)
				LOG_INF("ui ribbon: avg=%u us",aster_flux_draw_time_reset()/s_frame_n);
#endif
			}
			s_frame_us = s_wait_us = s_submit_us = 0;
			s_frame_n = s_frame_max_us = 0;
			LOG_INF("font prof: reads=%u B=%u warm=%u/%u miss=%u fill=%u evict=%u over=%u",
				fs.reads, (unsigned)(fs.read_bytes / 1024u),
				fs.warm_bursts, fs.warm_glyphs,
				fs.misses, fs.miss_filled, fs.evictions,
				fs.oversize);
			s_flush_n = s_flush_px = s_flush_bytes = 0;
			s_prof_ms = now;
		}
	}
}

lv_obj_t *aster_ui_lvgl_screen(void)
{
	return s_ready ? lv_screen_active() : NULL;
}

uint32_t aster_ui_lvgl_width(void)
{
	return s_w;
}

uint32_t aster_ui_lvgl_height(void)
{
	return s_h;
}

lv_indev_t *aster_ui_lvgl_indev(void)
{
	return s_indev;
}
