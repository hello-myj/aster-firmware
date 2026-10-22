#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * ASTER ABI native bridge: WASM import "env" symbol table -> ASTER services.
 *
 * Security model (design doc sec.61): every pointer arrives as a u32 app
 * offset. We resolve via wasm_runtime_addr_app_to_native() and validate the
 * full (off,len) against the instance memory BEFORE touching it. Handles are
 * type-tagged and index-checked. Nothing trusts the app.
 */
#include "aster_native.h"
#include "aster_runtime.h"
#include "aster_util.h"
#include "aster_logring.h"
#include "aster_storage.h"
#include "aster_sysinfo.h"
#include "aster_kv.h"
#include "aster_perms.h"
#include "aster_platform.h"
#include "aster_time.h"
#include "aster_mem.h"
#if IS_ENABLED(CONFIG_ASTER_IMGDEC)
#include "imgdec.h"
#endif
#if IS_ENABLED(CONFIG_ASTER_LVGL)
#include "ui_gfx.h"
#include "font_sd.h"
#include "qpanel.h"
#endif

#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <aster/aster.h>


LOG_MODULE_REGISTER(aster_native, LOG_LEVEL_INF);

/* Scratch budget for converting a non-RGB565 surface to RGB565 on present.
 *
 * A band costs its own size TWICE out of the DRAM heap: this scratch, plus
 * an identical-sized bounce buffer the SPI driver allocates for the
 * transfer itself (per call, because the source it is handed is the PSRAM
 * staging buffer and the DMA engine cannot read PSRAM). That heap is 64 KB
 * for the whole system, so 8 KB bands peak at 16 KB of scratch and will
 * fail outright if anything else holds memory at that moment; 4 KB bands
 * halve it, at the price of a few more CASET/RAMWR windows on a path the
 * RGB565 fast case never enters. See n_surface_present(). */
#define ASTER_PRESENT_BAND_BYTES 4096u

/* ---- helpers ---- */

#if IS_ENABLED(CONFIG_ASTER_LVGL)
/* True while the quick-settings sheet is up over this app and the glass is
 * therefore the sheet's, not the guest's: a present now would land on top of
 * it. Checked at the three wasm boundaries that END in a wire send.
 *
 * Deliberately here and not one level down, in
 * aster_plat_display_write_at_ex(): LVGL's own flush_cb reaches the panel
 * through that same door, so a check there would freeze the sheet along with
 * the app it is covering.
 *
 * The three call sites are placed so that only the TAIL of each call is
 * skipped -- rasterisation into the app's own surface has already run and
 * keeps running, because an app that reads its own pixels back must stay
 * self-consistent. And they return ASTER_OK, not an error: the user opened a
 * panel, and turning that into a wasm fault would kill the app for it. */
static bool glass_is_frozen(void)
{
	return aster_qpanel_app_holds_glass();
}
#endif

static aster_app_ctx_t *ctx_of(wasm_exec_env_t env)
{
	return aster_runtime_owner(wasm_runtime_get_module_inst(env));
}

/* ABI 1.2 permission gate: manifest-declared permissions are packed into
 * aster_app_rec_t.perm_mask at install/scan and hard-enforced. V1 left the
 * declarations informational; every in-tree sample declares correctly, and
 * the error text names the missing token, so external apps get a precise
 * fix instead of silent grandfathering. */
static bool aster_ctx_perm(aster_app_ctx_t *ctx, uint32_t bit)
{
	return ctx && ctx->rec && (ctx->rec->perm_mask & bit) != 0;
}

static int32_t sandbox_map(aster_app_ctx_t *ctx, const char *apath, char *real, size_t n)
{
	uint32_t perm = (ctx && ctx->rec) ? ctx->rec->perm_mask : 0;

	if (!ctx || aster_storage_sandbox(real, n, ctx->id, apath, perm) != 0) {
		return ASTER_EPERM;
	}
	return ASTER_OK;
}

/* Validate + resolve an app buffer of `len` bytes at `off`. */
static uint8_t *buf(wasm_exec_env_t env, uint32_t off, uint32_t len)
{
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
	if (!inst || off == 0 || !len) {
		return NULL;
	}
	if (!wasm_runtime_validate_app_addr(inst, off, len)) {
		return NULL;
	}
	return (uint8_t *)wasm_runtime_addr_app_to_native(inst, off);
}

/* copy an app string (offset + len) to a host buffer, NUL-terminated.
 * len == UINT32_MAX is the frozen "id_len or -1" form (abi/include/aster/
 * types.h, docs/abi-v1.md sec.7): the string runs up to its terminating NUL.
 * len == 0 is not that form and is rejected. */
static int app_str(wasm_exec_env_t env, uint32_t off, uint32_t len, char *out, size_t max)
{
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
	if (!inst || off == 0) {
		return -1;
	}
	if (len == UINT32_MAX) {
		/* NUL-terminated mode: the scan may not walk past the end of the
		 * linear memory, so bound it by the bytes that actually follow
		 * `off`. get_app_addr_range() reports that extent and, unlike
		 * validate_app_addr(), fails without arming a wasm trap. */
		uint64_t end = 0;
		if (!wasm_runtime_get_app_addr_range(inst, off, NULL, &end) ||
		    end <= off) {
			return -1;
		}
		size_t avail = (size_t)(end - off); /* bytes readable at off */
		size_t room = max ? max - 1 : 0;    /* keep one byte for the NUL */
		size_t lim = avail < room ? avail : room;
		uint8_t *base = buf(env, off, (uint32_t)lim);
		if (!base) {
			return -1;
		}
		size_t i = 0;
		while (i < lim && base[i] != 0) {
			out[i] = (char)base[i];
			i++;
		}
		out[i] = 0;
		return (int)i;
	}
	uint8_t *src = buf(env, off, len);
	if (!src || len + 1 > max) {
		return -1;
	}
	memcpy(out, src, len);
	out[len] = 0;
	return (int)len;
}

#include "aster_network_bridge.inc"
#include "aster_http_bridge.inc"
#include "aster_mqtt_bridge.inc"
#include "aster_ws_bridge.inc"
#include "aster_net_bridge.inc"

/* ---- handle decode ---- */

static int handle_index(uint32_t h, uint32_t tag, uint32_t max)
{
	if ((h & 0xFF000000u) != tag) {
		return -1;
	}
	uint32_t idx = (h & 0x00FFFFFFu);
	if (idx == 0 || idx > max) {
		return -1;
	}
	return (int)(idx - 1);
}

/* ============ system ============ */

static int32_t n_system_has_feature(wasm_exec_env_t env, uint32_t bit)
{
	ARG_UNUSED(env);
	return aster_sysinfo_has_feature(bit) ? 1 : 0;
}

static int32_t n_system_get_abi(wasm_exec_env_t env)
{
	ARG_UNUSED(env);
	return (int32_t)ASTER_ABI_VERSION;
}

/* ============ app / log / time ============ */

static void n_log(wasm_exec_env_t env, uint32_t level, uint32_t msg_off, uint32_t msg_len)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	char tmp[ASTER_LOG_MAX_MSG];
	int n = app_str(env, msg_off, msg_len, tmp, sizeof(tmp));
	if (n < 0) {
		return;
	}
	const char *id = ctx ? ctx->id : "app";
	aster_logring_write(id, level, tmp, (size_t)n);
	LOG_INF("[app:%s] %s", id, tmp);
}

static void n_app_exit(wasm_exec_env_t env, int32_t code)
{
	ARG_UNUSED(env);
	LOG_INF("app requested exit (%d)", code);
	aster_runtime_terminate();
}

static int32_t n_set_framerate(wasm_exec_env_t env, uint32_t fps)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (fps == 0) {
		fps = 30;
	}
	if (fps > 60) {
		fps = 60;
	}
	ctx->framerate = fps;
	return ASTER_OK;
}

static int32_t n_time_now_ms(wasm_exec_env_t env)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return 0;
	}
	uint64_t t = (uint64_t)k_uptime_get() - ctx->start_ms;
	return (int32_t)t;
}

static int32_t n_time_get(wasm_exec_env_t env, uint32_t out_off, uint32_t size)
{
	aster_time_info_t info;
	uint32_t copy_size;
	uint8_t *dst;

	if (size < 2 * sizeof(uint32_t)) {
		return ASTER_EINVAL;
	}
	copy_size = MIN(size, sizeof(info));
	dst = buf(env, out_off, copy_size);
	if (!dst) {
		return ASTER_EINVAL;
	}
	if (aster_time_read(&info) != 0) {
		return ASTER_EIO;
	}
	memcpy(dst, &info, copy_size);
	return ASTER_OK;
}

/* ============ timer ============ */

static int32_t n_timer_create(wasm_exec_env_t env, uint32_t period_ms, uint32_t repeat,
			      uint32_t user, uint32_t out_off)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	uint32_t *out = (uint32_t *)buf(env, out_off, 4);
	if (!ctx || !out) {
		return ASTER_EINVAL;
	}
	for (int i = 0; i < ASTER_TIMER_MAX; i++) {
		if (!ctx->timers[i].used) {
			ctx->timers[i].used = true;
			ctx->timers[i].period_ms = period_ms ? period_ms : 1;
			ctx->timers[i].repeat = repeat;
			ctx->timers[i].user = user;
			ctx->timers[i].next_ms = (uint64_t)k_uptime_get() + ctx->timers[i].period_ms;
			*out = ASTER_HTYPE_TIMER | (uint32_t)(i + 1);
			return ASTER_OK;
		}
	}
	return ASTER_ENOMEM;
}

static int32_t n_timer_destroy(wasm_exec_env_t env, uint32_t h)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	int i = handle_index(h, ASTER_HTYPE_TIMER, ASTER_TIMER_MAX);
	if (i < 0 || !ctx->timers[i].used) {
		return ASTER_EBADF;
	}
	ctx->timers[i].used = false;
	return ASTER_OK;
}

/* ============ display ============ */

static int32_t n_display_count(wasm_exec_env_t env)
{
	ARG_UNUSED(env);
	return aster_plat_display_count();
}

static int32_t n_display_get_info(wasm_exec_env_t env, uint32_t index, uint32_t info_off,
				  uint32_t info_size)
{
	uint8_t *dst = buf(env, info_off, info_size < sizeof(aster_display_info_t)
				   ? info_size : sizeof(aster_display_info_t));
	if (!dst) {
		return ASTER_EINVAL;
	}
	aster_display_info_t di;
	int err = aster_plat_display_get_info(index, &di);
	if (err) {
		return err;
	}
	size_t n = info_size < sizeof(di) ? info_size : sizeof(di);
	memcpy(dst, &di, n);
	return ASTER_OK;
}

/* ============ surface ============ */

static int32_t n_surface_create(wasm_exec_env_t env, uint32_t desc_off, uint32_t desc_size,
				uint32_t out_off)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	const aster_surface_desc_t *desc = (const aster_surface_desc_t *)
		buf(env, desc_off, desc_size < sizeof(aster_surface_desc_t)
			    ? desc_size : sizeof(aster_surface_desc_t));
	uint32_t *out = (uint32_t *)buf(env, out_off, 4);
	if (!ctx || !desc || !out) {
		return ASTER_EINVAL;
	}

	aster_display_info_t di;
	if (aster_plat_display_get_info(0, &di) != ASTER_OK) {
		return ASTER_ENOTSUP;
	}
	/* The default size, and the bound on an explicit one, are the APP's
	 * coordinate space, not the panel's (see aster_plat_display_app_space).
	 * On this glass that is 240x320 whichever way round the panel is, so
	 * an app that asks for nothing gets the shape it was written for
	 * rather than the shape the board booted into. */
	uint32_t aw, ah;

	if (!aster_plat_display_app_space(&aw, &ah)) {
		return ASTER_ENOTSUP;
	}
	/* PANEL_CANVAS already is the glass. Scaling it again would letterbox
	 * a buffer that was rasterized in panel pixels. */
	if ((desc->flags & ASTER_SURFACE_F_SCALE_FIT) &&
	    (desc->flags & ASTER_SURFACE_F_PANEL_CANVAS)) {
		return ASTER_EINVAL;
	}
	bool scale_fit = (desc->flags & ASTER_SURFACE_F_SCALE_FIT) != 0;
	/* The flag's buffer size is the app's coordinate space, so 0 is not
	 * "use the panel" -- the app has to say how big that space is. */
	if (scale_fit && (desc->width == 0 || desc->height == 0)) {
		return ASTER_EINVAL;
	}
	uint32_t w = desc->width ? desc->width : aw;
	uint32_t h = desc->height ? desc->height : ah;
	/* Either orientation of the app space is a legal request -- 240x320 is
	 * the space, 320x240 is the same glass the other way up. Anything
	 * else has no present path that could put it on the wire: the scaling
	 * present is written against the app space's own axes. A scale-fit
	 * surface is the exception: its size is chosen by the app and mapped
	 * onto the glass later, so it is not clipped to app space. */
	if (w == 0 || h == 0 ||
	    (scale_fit && (w > 4096u || h > 4096u)) ||
	    (!scale_fit && !((w <= aw && h <= ah) || (w <= ah && h <= aw)))) {
		return ASTER_EINVAL;
	}
	uint32_t fmt = desc->format;
	if (fmt == ASTER_PIXEL_NATIVE || fmt == 0) {
		fmt = di.native_format;
	}
	/* The accepted set is ASTER_FORMAT_TABLE, not a list retyped here. This
	 * gate and the converter it hands off to (aster_convert_to_rgb565) now
	 * read the same five rows, which is what stops the two from drifting
	 * again -- RGB888 was accepted here and implemented there, yet
	 * advertised nowhere, precisely because there were two lists.
	 * ASTER_PIXEL_NATIVE is already resolved above, so anything reaching
	 * this test is a concrete format. */
	if ((ASTER_FORMATS_CONVERTIBLE & ASTER_FMT_BIT(fmt)) == 0) {
		return ASTER_ENOTSUP;
	}
	/* bpp is the low byte of the format value by ABI design (see
	 * aster_pixel_format_t in aster/types.h), so reading it beats keeping a
	 * second per-format table -- the old switch here would have handed any
	 * newly added format 4 bytes through its default arm. */
	uint32_t bpp = fmt & 0xFFu;

	uint32_t size = w * h * bpp;
	/* A panel-order canvas has no app-side pixels at all: the engine draws
	 * straight into the platform's front buffer so presenting needs no
	 * conversion sweep. Grant it only for the shape it was designed for
	 * (565, full screen, unrotated panel); anything else silently gets the
	 * ordinary surface, which is always correct, just slower. */
	/* A canvas is the panel's own buffer, so it is only ever the right
	 * answer when the app's surface IS the panel -- which is what
	 * canvas_ready() now also checks (it is false whenever the platform
	 * is scaling). The w/h test against di is therefore redundant on this
	 * board and kept only because it is the cheap half. */
	bool want_canvas = (desc->flags & ASTER_SURFACE_F_PANEL_CANVAS) &&
			   fmt == ASTER_PIXEL_RGB565 && w == di.width && h == di.height;
	bool fronts = false;

	if (want_canvas && aster_plat_display_flip_acquire() == 0) {
		fronts = true;
	}
	bool canvas = want_canvas && aster_plat_display_canvas_ready();

	if (fronts && !canvas) {
		aster_plat_display_flip_release();
	}

	/* The denial is silent by design (the app still works, just slower),
	 * but that silence is costly when diagnosing "this app's colours are
	 * off": a panel canvas writes panel byte order and an ordinary one
	 * writes ABI order, so which one a surface got decides how its
	 * presents are converted. Say it once, per surface. */
	if (desc->flags & ASTER_SURFACE_F_PANEL_CANVAS) {
		LOG_INF("surface: %ux%u panel-canvas requested -> %s", w, h,
			canvas ? "granted (engine writes panel order)"
			       : "denied, ordinary LE surface");
	}

	if (canvas) {
		size = 0;
	}
	for (int i = 0; i < ASTER_SURFACE_MAX; i++) {
		if (!ctx->surfaces[i].used) {
			void *native = NULL;
			uint32_t offset = 0;

			if (!canvas) {
				offset = (uint32_t)wasm_runtime_module_malloc(
					ctx->module_inst, size, &native);
				if (!offset || !native) {
					return ASTER_ENOMEM;
				}
				memset(native, 0, size);
			}
			ctx->surfaces[i].used = true;
			ctx->surfaces[i].app_offset = offset;
			ctx->surfaces[i].size = size;
			ctx->surfaces[i].w = w;
			ctx->surfaces[i].h = h;
			ctx->surfaces[i].pitch = w;
			ctx->surfaces[i].format = fmt;
			ctx->surfaces[i].canvas = canvas;
			ctx->surfaces[i].scale_fit = scale_fit;
			*out = ASTER_HTYPE_SURFACE | (uint32_t)(i + 1);
			return ASTER_OK;
		}
	}
	if (canvas) {
		aster_plat_display_flip_release();
	}
	return ASTER_ENOMEM;
}

static int32_t n_surface_destroy(wasm_exec_env_t env, uint32_t h)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	int i = handle_index(h, ASTER_HTYPE_SURFACE, ASTER_SURFACE_MAX);
	if (i < 0) {
		return ASTER_EBADF;
	}
 if(ctx->gfx_frame.published_surface==h) ctx->gfx_frame.published_surface=0;
 if(ctx->gfx_frame.cache_surface==h) aster_frame_release_cache(&ctx->gfx_frame,aster_psram_free);
 if(ctx->gfx_published_surface==h) {
  ctx->gfx_published_surface=0;
#if IS_ENABLED(CONFIG_ASTER_LVGL)
  aster_gfx_note_committed_frame(NULL,0,0,0,false);
#endif
 }
	if (ctx->surfaces[i].used) {
		/* The flip path snapshots into its own front buffers, but a
		 * still-queued send could in principle refer to app memory on
		 * the staging path. Drain before handing the pages back so the
		 * worker can never read freed linear memory. */
		aster_plat_display_flush_drain();
		if (ctx->surfaces[i].canvas) {
			aster_plat_display_flip_release();
		}
		if (ctx->surfaces[i].app_offset) {
			wasm_runtime_module_free(ctx->module_inst,
						 ctx->surfaces[i].app_offset);
		}
		bool was_fit = ctx->surfaces[i].scale_fit;

		ctx->surfaces[i].used = false;
		ctx->surfaces[i].canvas = false;
		ctx->surfaces[i].scale_fit = false;
		if (was_fit) {
			aster_plat_display_scale_fit_disarm();
		}
	}
	return ASTER_OK;
}

static int32_t n_surface_acquire(wasm_exec_env_t env, uint32_t h, uint32_t buf_off,
				 uint32_t buf_size)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	uint8_t *dst = buf(env, buf_off, buf_size < sizeof(aster_surface_buffer_t)
				   ? buf_size : sizeof(aster_surface_buffer_t));
	if (!ctx || !dst) {
		return ASTER_EINVAL;
	}
	int i = handle_index(h, ASTER_HTYPE_SURFACE, ASTER_SURFACE_MAX);
	if (i < 0 || !ctx->surfaces[i].used) {
		return ASTER_EBADF;
	}
	aster_surface_buffer_t sb = {
		.size = sizeof(sb),
		.version = 1,
		.ptr = ctx->surfaces[i].canvas
			       ? NULL /* firmware-owned canvas */
			       : (void *)(uintptr_t)ctx->surfaces[i].app_offset,
		.width = ctx->surfaces[i].w,
		.height = ctx->surfaces[i].h,
		.pitch = ctx->surfaces[i].w * ((ctx->surfaces[i].format == ASTER_PIXEL_RGB565 ||
						ctx->surfaces[i].format == ASTER_PIXEL_BGR565) ? 2
					: (ctx->surfaces[i].format == ASTER_PIXEL_RGB888 ? 3 : 4)),
		.format = ctx->surfaces[i].format,
		.flags = (ctx->surfaces[i].canvas ? ASTER_SURFACE_F_PANEL_CANVAS : 0u) |
			 (ctx->surfaces[i].scale_fit ? ASTER_SURFACE_F_SCALE_FIT : 0u),
	};
	size_t n = buf_size < sizeof(sb) ? buf_size : sizeof(sb);
	memcpy(dst, &sb, n);
	return ASTER_OK;
}

/* Scale-fit present. RGB565 is sampled directly. Any other accepted format is
 * converted once into a firmware RGB565 buffer, then scaled with the same
 * keep map. The conversion buffer is not the glass; the platform writes the
 * scaled frame into the existing full-frame present path. */
static int32_t present_scale_fit(aster_surface_t *s, const uint8_t *src)
{
	if (s->format == ASTER_PIXEL_RGB565) {
		return aster_plat_display_scale_fit_present(src, s->w, s->h, s->pitch);
	}
	uint32_t bpp = (s->format == ASTER_PIXEL_BGR565) ? 2
			 : (s->format == ASTER_PIXEL_RGB888 ? 3 : 4);
	size_t bytes = (size_t)s->w * s->h * 2u;
	uint16_t *tmp = aster_psram_alloc(bytes);

	if (!tmp) {
		return ASTER_ENOMEM;
	}
	int err = aster_convert_to_rgb565(src, s->w * bpp, s->format, tmp, s->w * 2u,
					  s->w, s->h);

	if (!err) {
		err = aster_plat_display_scale_fit_present((const uint8_t *)tmp, s->w, s->h,
							   s->w);
	}
	aster_psram_free(tmp);
	return err;
}

static int32_t n_surface_present(wasm_exec_env_t env, uint32_t h)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
 if(ctx->gfx_frame.state) return aster_frame_fail(&ctx->gfx_frame,ASTER_EBUSY);
 ctx->gfx_published_surface=0;
	int i = handle_index(h, ASTER_HTYPE_SURFACE, ASTER_SURFACE_MAX);
	if (i < 0 || !ctx->surfaces[i].used) {
		return ASTER_EBADF;
	}
#if IS_ENABLED(CONFIG_ASTER_LVGL)
	if (glass_is_frozen()) {
		return ASTER_OK; /* sheet is up: present swallowed whole */
	}
#endif
	aster_surface_t *s = &ctx->surfaces[i];
	if (s->canvas) {
		/* Nothing to read back: the frame is already sitting in a
		 * panel-order front buffer, so this is just "hand it over". */
		return aster_plat_display_canvas_present();
	}
	if (s->scale_fit) {
		uint8_t *fit = (uint8_t *)wasm_runtime_addr_app_to_native(ctx->module_inst,
									  s->app_offset);

		if (!fit) {
			return ASTER_EBADF;
		}
		return present_scale_fit(s, fit);
	}
	aster_plat_display_scale_fit_disarm();
	uint8_t *src = (uint8_t *)wasm_runtime_addr_app_to_native(ctx->module_inst,
								  s->app_offset);
	if (!src) {
		return ASTER_EBADF;
	}
	uint32_t bpp = (s->format == ASTER_PIXEL_RGB565 || s->format == ASTER_PIXEL_BGR565) ? 2
			 : (s->format == ASTER_PIXEL_RGB888 ? 3 : 4);
	if (s->format == ASTER_PIXEL_RGB565) {
		/* Whole-screen RGB565, in the APP's coordinate space -- so this
		 * is the app-present entry, not the panel one. When app space is
		 * the panel (portrait here) the platform forwards straight to the
		 * 1:1 path and behaviour is unchanged; when it is not, this is
		 * where the frame gets scaled and centred. Either way the async
		 * double-buffered flip is chosen inside the platform, so this call
		 * takes it automatically and only falls back to a synchronous
		 * send for sub-screen or rotated presents. */
		return aster_plat_display_app_present(src, s->w, s->h, s->pitch);
	}
	/* Anything else converts to RGB565 first, a band at a time.
	 *
	 * The obvious version -- one w*h*2 scratch for the whole frame -- asks
	 * the 65,536 B DRAM heap for 64,800 B at the common 240x135 canvas,
	 * i.e. 98.9% of it, and fails outright if anything else holds more
	 * than ~700 B (the display's own band buffer alone takes 7,680 B).
	 * Bands cap the request at ASTER_PRESENT_BAND_BYTES regardless of
	 * canvas size.
	 *
	 * Not PSRAM instead: the panel link is driven by GDMA, which cannot
	 * read PSRAM, so a PSRAM scratch would buy the same DRAM staging
	 * buffer back as a bounce copy -- more internal RAM traffic, not less.
	 *
	 * The cost is one CASET/RASET/RAMWR window per band instead of one
	 * per frame, on a path the RGB565 fast case above never enters. The
	 * paths a whole-frame call would have reached are not lost either:
	 * the async flip wants a full 240x320 rect -- 153,600 B -- which this
	 * 65,536 B heap never held, so this branch only ever returned ENOMEM
	 * for a full-screen surface. Bands are what make it work at all. */
	uint32_t rows = ASTER_PRESENT_BAND_BYTES / (s->w * 2u);

	if (rows == 0) {
		rows = 1;
	}
	uint16_t *tmp = k_malloc((size_t)rows * s->w * 2u);

	if (!tmp) {
		return ASTER_ENOMEM;
	}
	int err = ASTER_OK;

	for (uint32_t y = 0; y < s->h; y += rows) {
		uint32_t n = MIN(rows, s->h - y);

		err = aster_convert_to_rgb565(src + (size_t)y * s->w * bpp, s->w * bpp,
					    s->format, tmp, s->w * 2u, s->w, n);
		if (err) {
			break;
		}
		/* pitch is in PIXELS here (write_at_ex multiplies by 2 itself),
		 * not the bytes/row that aster_convert_to_rgb565 takes above. And the
		 * rect is in APP space, like every other surface rect: the platform
		 * maps it onto the panel. */
		err = aster_plat_display_write_app_at((const uint8_t *)tmp, 0, y,
						    s->w, n, s->w);
		if (err) {
			break;
		}
	}
	k_free(tmp);
	return err;
}

/* ABI 1.4: region present. src is any RGB565 buffer in app linear memory
 * (LVGL PARTIAL draw buffer), pushed to the display at (x,y). Bounds-checked
 * against the surface so an app cannot scribble outside its own window. */
static int32_t n_surface_flush(wasm_exec_env_t env, uint32_t h, uint32_t src_off,
			       uint32_t x, uint32_t y, uint32_t w, uint32_t hh,
			       uint32_t src_pitch)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
 if(ctx->gfx_frame.state) return aster_frame_fail(&ctx->gfx_frame,ASTER_EBUSY);
 ctx->gfx_published_surface=0;
	int i = handle_index(h, ASTER_HTYPE_SURFACE, ASTER_SURFACE_MAX);
	if (i < 0 || !ctx->surfaces[i].used) {
		return ASTER_EBADF;
	}
#if IS_ENABLED(CONFIG_ASTER_LVGL)
	if (glass_is_frozen()) {
		return ASTER_OK; /* sheet is up: this region does not reach the panel */
	}
#endif
	aster_surface_t *s = &ctx->surfaces[i];
	if (s->scale_fit) {
		/* A scale-fit surface is presented as a whole keep-aspect frame.
		 * A 1:1 region push would ignore the margins and the scale. */
		return ASTER_ENOTSUP;
	}
	if (s->format != ASTER_PIXEL_RGB565) {
		return ASTER_ENOTSUP; /* flush is the fast 565-only path by design */
	}
	/* x/y/w/hh are raw app u32s: compare without forming x + w, which
	 * wraps and would let a bogus rect reach the display layer. */
	if (!w || !hh || x > s->w || w > s->w - x || y > s->h || hh > s->h - y) {
		return ASTER_EINVAL;
	}
	if (!src_pitch) {
		src_pitch = w;
	}
	if (src_pitch < w) {
		return ASTER_EINVAL;
	}
	/* src holds a packed w x hh region; y is the DESTINATION coordinate on
	 * the SURFACE (app space, not the panel), NOT an offset into src. Size
	 * the linear-memory read window by rows of the region only. */
	uint64_t need = (uint64_t)(hh - 1) * src_pitch * 2 + (uint64_t)w * 2;
	if (need >= UINT32_MAX) {
		return ASTER_EINVAL; /* buf(env,..) takes a u32 span */
	}
	uint8_t *src = (uint8_t *)buf(env, src_off, (uint32_t)need);
	if (!src) {
		return ASTER_EINVAL;
	}
	/* write_app_at's pitch is pixels-per-row, same unit as src_pitch. */
	return aster_plat_display_write_app_at(src, x, y, w, hh, src_pitch);
}

/* Guest memory growth may relocate an ordinary surface. Resolve the LAST
 * committed surface on the worker before screenshots; never expose staging. */
void aster_native_frame_refresh(aster_app_ctx_t *ctx)
{
#if IS_ENABLED(CONFIG_ASTER_LVGL)
 if(!ctx||!ctx->module_inst||!ctx->gfx_published_surface) return;
 int i=handle_index(ctx->gfx_published_surface,ASTER_HTYPE_SURFACE,ASTER_SURFACE_MAX);
 if(i<0||!ctx->surfaces[i].used) return;
 aster_surface_t *s=&ctx->surfaces[i];
 if(s->canvas) return;
 const uint8_t *pixels=wasm_runtime_addr_app_to_native(ctx->module_inst,s->app_offset);
 aster_gfx_note_committed_frame(pixels,s->w,s->h,s->pitch,false);
#else
 (void)ctx;
#endif
}

void aster_native_frame_abort(aster_app_ctx_t *ctx)
{
 if(ctx) {aster_frame_abort(&ctx->gfx_frame);ctx->gfx_frame.published_surface=0;}
}
void aster_native_frame_release(aster_app_ctx_t *ctx)
{
 if(!ctx) return;
 aster_native_frame_abort(ctx);
 ctx->gfx_published_surface=0;
 aster_frame_release_cache(&ctx->gfx_frame,aster_psram_free);
}

/* ============ gfx command engine (ABI 2.2) ============ */
#if IS_ENABLED(CONFIG_ASTER_LVGL)

static int32_t gfx_surface(wasm_exec_env_t env, uint32_t h, aster_surface_t **out)
{
	aster_app_ctx_t *ctx = ctx_of(env);

	if (!ctx) {
		return ASTER_EBADF;
	}
	int i = handle_index(h, ASTER_HTYPE_SURFACE, ASTER_SURFACE_MAX);

	if (i < 0 || !ctx->surfaces[i].used) {
		return ASTER_EBADF;
	}
	if (ctx->surfaces[i].format != ASTER_PIXEL_RGB565) {
		return ASTER_ENOTSUP; /* the engine is 565-only by design */
	}
	*out = &ctx->surfaces[i];
	return ASTER_OK;
}

static int32_t n_gfx_frame_control(wasm_exec_env_t env,uint32_t h,
 aster_surface_t *s,uint32_t off,uint32_t len)
{
 aster_app_ctx_t *ctx=ctx_of(env);aster_frame_state_t *f=&ctx->gfx_frame;
 if(!off||(off&3u)) return ASTER_EINVAL;
 if(len==sizeof(aster_gfx_frame_clip_t)) {
  const aster_gfx_frame_clip_t *clip=(const aster_gfx_frame_clip_t *)buf(env,off,len);
  if(!clip||clip->size!=sizeof(*clip)||clip->version!=1||clip->action!=ASTER_GFX_FRAME_SET_CLIP) return ASTER_EINVAL;
  return aster_frame_set_clip(f,h,clip,k_uptime_get());
 }
 if(len!=sizeof(aster_gfx_frame_control_t)) return ASTER_EINVAL;
 aster_gfx_frame_control_t *out=(aster_gfx_frame_control_t *)buf(env,off,len);
 if(!out) return ASTER_EINVAL;
 aster_gfx_frame_control_t c=*out;
 if(c.size!=sizeof(c)||c.version!=1||c.flags||c.error) return ASTER_EINVAL;
 if(c.action==ASTER_GFX_FRAME_QUERY) {
  if(c.frame_id) return ASTER_EINVAL;
  (void)aster_font_sd_repaint_pending();
  out->flags=ASTER_GFX_FRAME_SUPPORTED|ASTER_GFX_FRAME_CLIP_SUPPORTED|ASTER_GFX_FRAME_FONT_EPOCH_SUPPORTED;
  out->frame_id=aster_font_sd_generation();return 0;
 }
 if(c.action==ASTER_GFX_FRAME_BEGIN) {
  if(c.frame_id) return ASTER_EINVAL;
  if(ctx->paused||glass_is_frozen()) return ASTER_EBUSY;
  const uint8_t *base=s->canvas ? NULL : wasm_runtime_addr_app_to_native(ctx->module_inst,s->app_offset);
  int e=aster_frame_start(f,h,s->w,s->h,s->canvas,base,s->pitch,k_uptime_get(),aster_psram_alloc,aster_psram_free);
  if(e) return e;
  (void)aster_plat_display_flush_drain_checked();
  out->frame_id=f->id;return 0;
 }
 if(f->state!=ASTER_FRAME_DRAWING||f->surface!=h||c.frame_id!=f->id) return ASTER_ESTATE;
 if(c.action==ASTER_GFX_FRAME_CANCEL) {aster_frame_abort(f);return 0;}
 if(c.action!=ASTER_GFX_FRAME_END) return ASTER_EINVAL;
 int e=aster_frame_check(f,k_uptime_get());
 if(!e && !f->initialized) e=ASTER_ESTATE;
 if(!e && (ctx->paused||glass_is_frozen())) e=ASTER_EBUSY;
 if(e) {out->error=e;aster_frame_abort(f);return e;}
 /* Guest memory may have grown after BEGIN. Resolve it again at END. */
 uint8_t *guest=s->canvas ? NULL : wasm_runtime_addr_app_to_native(ctx->module_inst,s->app_offset);
 if(!s->canvas&&!guest) {aster_frame_abort(f);return ASTER_EBADF;}
 f->state=ASTER_FRAME_QUEUED;
 aster_plat_display_flush_drain();
 f->state=ASTER_FRAME_IN_FLIGHT;
 uint8_t *committed=guest;
 if(s->canvas) {
  uint8_t *back=aster_plat_display_canvas_back();
  committed=back;
  if(!back) e=ASTER_ENOTSUP;
  else {
   for(uint32_t i=0;i<f->w*f->h;i++) {back[i*2u]=f->pixels[i*2u+1];back[i*2u+1]=f->pixels[i*2u];}
   e=aster_plat_display_canvas_present();
  }
 } else if(s->scale_fit) e=present_scale_fit(s,f->pixels);
 else {
  aster_plat_display_scale_fit_disarm();
  /* A retained damage frame is usable only while nobody else has written
   * the panel. Overlay, orientation, legacy writes and I/O failure force a
   * full restoration. The first END always publishes the complete surface. */
  bool retained=f->published_surface==h &&
   f->published_generation==aster_plat_display_generation();
  bool full=aster_dirty_is_full(&f->dirty,f->w,f->h);
  if(!retained||full) e=aster_plat_display_app_present(f->pixels,f->w,f->h,f->w);
  else e=aster_plat_display_write_app_regions(f->pixels,f->w,f->h,f->w,&f->dirty);
 }
 int completion=aster_plat_display_flush_drain_checked();
 if(!e) e=completion;
 if(!e && guest) aster_frame_commit_guest(f,guest,s->pitch);
 if(!e) {
  ctx->gfx_published_surface=h;
  f->published_surface=h;f->published_generation=aster_plat_display_generation();
  aster_gfx_note_committed_frame(committed,s->w,s->h,s->pitch,s->canvas);
 }
 /* A failed END may have changed either the shadow or a physical prefix.
  * Restore from Guest in full next time, even if no wire was attempted. */
 if(e) f->published_surface=0;
 out->error=e;aster_frame_abort(f);return e;
}

static int32_t n_gfx_exec(wasm_exec_env_t env, uint32_t h, uint32_t cmds_off,
			  uint32_t n, uint32_t arena_off, uint32_t arena_len,
			  uint32_t px, uint32_t py, uint32_t pw, uint32_t ph)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	aster_surface_t *s;
	int32_t e = gfx_surface(env, h, &s);

	if (e != ASTER_OK) {
		return ctx ? aster_frame_fail(&ctx->gfx_frame,e) : e;
	}
 if(n==ASTER_GFX_FRAME_CONTROL && cmds_off==0 && px==ASTER_GFX_FRAME_MAGIC && py==ASTER_GFX_FRAME_VERSION && pw==0 && ph==0)
  return n_gfx_frame_control(env,h,s,arena_off,arena_len);
 aster_frame_state_t *frame=&ctx->gfx_frame;
 bool frame_batch=py==ASTER_GFX_FRAME_MAGIC && pw==0 && ph==0;
 if(frame_batch && !frame->state) return ASTER_ESTATE;
 if(frame_batch && n && (cmds_off&3u)) return aster_frame_fail(frame,ASTER_EINVAL);
 if(frame->state) {
  if(!frame_batch||px!=frame->id) return aster_frame_fail(frame,ASTER_ESTATE);

  e=aster_frame_check(frame,k_uptime_get());if(e) return e;
  if(frame->surface!=h) return aster_frame_fail(frame,ASTER_EBUSY);
 }
	/* Reject invalid explicit rectangles before touching Guest pixels. */
	if ((pw != 0u) != (ph != 0u) || (pw && (px > s->w || pw > s->w-px || py > s->h || ph > s->h-py)))
		return aster_frame_fail(&ctx->gfx_frame,ASTER_EINVAL);
	if (n > 4096u || (uint64_t)n * sizeof(aster_gfx_cmd_t) > UINT32_MAX) {
		return aster_frame_fail(&ctx->gfx_frame,ASTER_EINVAL);
	}
	const aster_gfx_cmd_t *cmds = NULL;

	if (n) {
		cmds = (const aster_gfx_cmd_t *)buf(env, cmds_off,
						  n * sizeof(aster_gfx_cmd_t));
		if (!cmds) {
			return aster_frame_fail(&ctx->gfx_frame,ASTER_EINVAL);
		}
	}
	const uint8_t *arena = NULL;

	if (arena_len) {
		if (arena_off == 0u) {
			return aster_frame_fail(&ctx->gfx_frame,ASTER_EINVAL);
		}
		arena = buf(env, arena_off, arena_len);
		if (!arena) {
			return aster_frame_fail(&ctx->gfx_frame,ASTER_EINVAL);
		}
	}
 if(frame->state) {
  e=aster_frame_batch(frame,h,cmds,n,arena_len,k_uptime_get());if(e) return e;
  uint32_t dx=0,dy=0,dw=0,dh=0;
  aster_gfx_set_clip(frame->clip_x,frame->clip_y,frame->clip_w,frame->clip_h);
  int ran=aster_gfx_exec_unpublished_into(frame->pixels,frame->w,frame->h,frame->w,cmds,n,arena,arena_len,&dx,&dy,&dw,&dh);
  aster_gfx_set_clip(0,0,0,0);
  if(ran<0) return aster_frame_fail(frame,ran);
  aster_dirty_set_t batch;aster_gfx_last_dirty(&batch);
  aster_dirty_append(&frame->dirty,&batch,frame->w,frame->h);
  return ran;
 }
 ctx->gfx_published_surface=0;
	/* Where the engine draws. An ordinary surface lives in app linear
	 * memory and holds ABI-order words; a panel canvas lives in the
	 * platform's front buffer, already in the byte order the glass wants,
	 * which is what removes the per-frame conversion sweep. */
	uint8_t *base;
	bool out_be = s->canvas;

	if (s->canvas) {
		base = aster_plat_display_canvas_back();
		if (!base) {
			return ASTER_ENOTSUP;
		}
	} else {
		base = (uint8_t *)wasm_runtime_addr_app_to_native(ctx->module_inst,
								  s->app_offset);
		if (!base) {
			return ASTER_EBADF;
		}
	}
	/* whole-buffer pixel span is what exec_into may scribble: revalidate
	 * it against the surface bounds (creation already did) */
	uint32_t dx = 0, dy = 0, dw = 0, dh = 0;
	bool frame_clear = false;

	if (!s->canvas && pw && ph) {
		for (uint32_t i = 0; i < n; i++) {
			if (ASTER_GFX_OP(&cmds[i]) == ASTER_GFX_CLEAR) {
				frame_clear = true;
				break;
			}
		}
		if (!frame_clear) {
			aster_gfx_set_clip(px, py, pw, ph);
		}
	}

#if IS_ENABLED(CONFIG_ASTER_GFX_TIMING)
	uint32_t t_raster0 = k_cycle_get_32();
#endif
	int32_t ran = aster_gfx_exec_into(base, s->w, s->h, s->pitch, cmds, n, arena,
					arena_len, &dx, &dy, &dw, &dh, out_be);
	aster_gfx_set_clip(0, 0, 0, 0);
#if IS_ENABLED(CONFIG_ASTER_GFX_TIMING)
	uint32_t t_flush0 = k_cycle_get_32();
	uint32_t acc_raster = t_flush0 - t_raster0, acc_flush = 0, flushed_px = 0;
#endif

	if (ran < 0) {
		return ASTER_EBADF;
	}
    aster_dirty_set_t regions;aster_gfx_last_dirty(&regions);
	/* explicit present rect wins; else the accumulated dirty box */
	if (pw && ph) {
		/* same no-wrap form as n_surface_flush: px/pw are app u32s and
		 * px + pw would wrap straight past the surface bounds */
		if (px > s->w || pw > s->w - px || py > s->h || ph > s->h - py) {
			return aster_frame_fail(&ctx->gfx_frame,ASTER_EINVAL);
		}
		dx = px;
		dy = py;
		dw = pw;
		dh = ph;
	}
#if IS_ENABLED(CONFIG_ASTER_LVGL)
	/* Checked HERE, not at the top of the function: aster_gfx_exec_into above
	 * has already rasterised into the app's own surface, and that is
	 * deliberate -- the app is still running, and a frame it draws and then
	 * reads back must be the frame it drew. Only the present is skipped. */
	if (glass_is_frozen()) {
		return ASTER_OK;
	}
#endif
	if (dw && dh) {
		int err;

		if (!s->scale_fit) {
			aster_plat_display_scale_fit_disarm();
		}
		if (s->scale_fit) {
			err = present_scale_fit(s, base);
		} else if (s->canvas) {
			/* A canvas present only works whole-screen: the two front
			 * buffers alternate, so a partial frame would leave the
			 * untouched rows holding a frame from two presents ago.
			 * An app that reaches here repainted less than the full
			 * canvas -- its own contract violation, but we can still
			 * put the right pixels on the glass, just without the
			 * hand-off. Warn once so it is not a silent slow path. */
			if (dx != 0 || dy != 0 || dw != s->w || dh != s->h) {
				static bool warned;

				if (!warned) {
					warned = true;
					LOG_WRN("panel canvas surface needs a full-frame "
						"redraw per present (got %ux%u at %u,%u); "
						"falling back to a rect send",
						(unsigned)dw, (unsigned)dh, (unsigned)dx,
						(unsigned)dy);
				}
				uint8_t *src = base + ((size_t)dy * s->pitch + dx) * 2u;

				err = aster_plat_display_write_at_ex(src, dx, dy, dw, dh,
								   s->pitch, true);
				/* The rect send goes out through the shared async
				 * slot, which the next frame's engine render would
				 * race; serialise rather than tear. */
				aster_plat_display_flush_drain();
			} else {
				err = aster_plat_display_canvas_present();
			}
		} else if (dx == 0 && dy == 0 && dw == s->w && dh == s->h &&
                   ((pw&&ph)||aster_dirty_is_full(&regions,s->w,s->h))) {
			/* Whole surface: the app-present entry, which gets the
			 * double-buffered async flip (and, when the app's space is
			 * not the panel's, the scale) for free. */
			err = aster_plat_display_app_present(base, s->w, s->h, s->pitch);
		} else {
			/* A dirty rect, in APP coordinates: the platform maps it
			 * onto the panel, which is an identity when the two spaces
			 * agree and a scaled/offset rect when they do not. */
			uint8_t *src = base + ((size_t)dy * s->pitch + dx) * 2u;

			if(pw&&ph) err=aster_plat_display_write_app_at(src,dx,dy,dw,dh,s->pitch);
            else {
             err=aster_plat_display_write_app_regions(base,s->w,s->h,s->pitch,&regions);
            }
		}
#if IS_ENABLED(CONFIG_ASTER_GFX_TIMING)
		acc_flush = k_cycle_get_32() - t_flush0;
		flushed_px = dw * dh;
#endif
		if (err) {
			return err;
		}
	}
#if IS_ENABLED(CONFIG_ASTER_GFX_TIMING)
	/* One frame per call in the common case. The split is measured here
	 * because the rasterization runs *inside* aster_gfx_exec(): an app that
	 * times around the call reports raster+flush as one number and will
	 * attribute the bottleneck to the wrong half. */
	{
		static uint32_t s_n, s_ras, s_flu, s_px, s_cmds;
		static uint32_t s_ras_max, s_flu_max;

		s_n++;
		s_ras += acc_raster;
		s_flu += acc_flush;
		s_px += flushed_px;
		s_cmds += n;
		if (acc_raster > s_ras_max) {
			s_ras_max = acc_raster;
		}
		if (acc_flush > s_flu_max) {
			s_flu_max = acc_flush;
		}
		if (s_n >= 64u) {
			uint32_t ras = k_cyc_to_us_floor32(s_ras) / 64u;
			uint32_t flu = k_cyc_to_us_floor32(s_flu) / 64u;

			/* Printed from a worker thread: a log line written
			 * here would stall the render thread (a console
			 * write costs milliseconds) and this frame is
			 * already at the budget's edge. That worker also
			 * emits the per-op raster histogram so the
			 * aggregate can be attributed to an op. */
			aster_gfx_timing_report(ras, flu,
				(uint32_t)k_cyc_to_us_floor32(s_ras_max),
				(uint32_t)k_cyc_to_us_floor32(s_flu_max),
				(uint32_t)(s_cmds / 64u), (uint32_t)(s_px / 64u));
			s_n = s_ras = s_flu = s_px = s_cmds = 0;
			s_ras_max = s_flu_max = 0;
		}
	}
#endif
	return ran;
}

static int32_t n_gfx_text_w(wasm_exec_env_t env, uint32_t px, uint32_t off,
			    uint32_t len)
{
	if (px < 1u || px > 64u || len == 0u) {
		return ASTER_EINVAL;
	}
	char *s = (char *)buf(env, off, len);

	if (!s) {
		return ASTER_EINVAL;
	}
	return aster_gfx_text_measure((uint8_t)px, s, len);
}

static int32_t n_gfx_fonts_pending(wasm_exec_env_t env)
{
	ARG_UNUSED(env);
	return aster_font_sd_repaint_pending() ? 1 : 0;
}

static int32_t n_gfx_bmp_load(wasm_exec_env_t env, uint32_t path_off,
			       uint32_t path_len, uint32_t cap_w, uint32_t cap_h,
			       uint32_t oh_off, uint32_t ow_off, uint32_t oht_off)
{
	aster_app_ctx_t *ctx = ctx_of(env);

	if (!ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	char apath[160];

	if (app_str(env, path_off, path_len, apath, sizeof(apath)) < 0) {
		return ASTER_EINVAL;
	}
	char real[256];

	if (sandbox_map(ctx, apath, real, sizeof(real)) != 0) {
		return ASTER_EPERM;
	}
	if (!cap_w || !cap_h || cap_w > 320u || cap_h > 320u) {
		return ASTER_EINVAL;
	}
	uint32_t *oh = (uint32_t *)buf(env, oh_off, 4);
	uint32_t *ow = (uint32_t *)buf(env, ow_off, 4);
	uint32_t *oht = (uint32_t *)buf(env, oht_off, 4);

	if (!oh || !ow || !oht) {
		return ASTER_EINVAL;
	}
#if IS_ENABLED(CONFIG_ASTER_IMGDEC)
	return aster_gfx_bmp_load_into(real, cap_w, cap_h, oh, ow, oht);
#else
	ARG_UNUSED(real);
	return ASTER_ENOTSUP;
#endif
}

static int32_t n_gfx_bmp_free(wasm_exec_env_t env, uint32_t handle)
{
	ARG_UNUSED(env);
	return aster_gfx_bmp_release(handle);
}

#else /* !CONFIG_ASTER_LVGL: symbols stay registered, refuse at runtime */

static int32_t n_gfx_exec(wasm_exec_env_t env, uint32_t h, uint32_t c, uint32_t n,
			  uint32_t a, uint32_t al, uint32_t x, uint32_t y,
			  uint32_t w, uint32_t hh)
{
	ARG_UNUSED(env); ARG_UNUSED(h); ARG_UNUSED(c); ARG_UNUSED(n);
	ARG_UNUSED(a); ARG_UNUSED(al); ARG_UNUSED(x); ARG_UNUSED(y);
	ARG_UNUSED(w); ARG_UNUSED(hh);
	return ASTER_ENOTSUP;
}

static int32_t n_gfx_text_w(wasm_exec_env_t env, uint32_t px, uint32_t off,
			    uint32_t len)
{
	ARG_UNUSED(env); ARG_UNUSED(px); ARG_UNUSED(off); ARG_UNUSED(len);
	return ASTER_ENOTSUP;
}

static int32_t n_gfx_fonts_pending(wasm_exec_env_t env)
{
	ARG_UNUSED(env);
	/* no LVGL -> no cpfont pipeline -> never anything to wait for */
	return 0;
}

static int32_t n_gfx_bmp_load(wasm_exec_env_t env, uint32_t a, uint32_t b,
			       uint32_t c, uint32_t d, uint32_t e, uint32_t f,
			       uint32_t g)
{
	ARG_UNUSED(env); ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	ARG_UNUSED(d); ARG_UNUSED(e); ARG_UNUSED(f); ARG_UNUSED(g);
	return ASTER_ENOTSUP;
}

static int32_t n_gfx_bmp_free(wasm_exec_env_t env, uint32_t handle)
{
	ARG_UNUSED(env); ARG_UNUSED(handle);
	return ASTER_ENOTSUP;
}
#endif

/* ============ input ============ */

static int32_t n_input_get_info(wasm_exec_env_t env, uint32_t info_off, uint32_t info_size)
{
	uint8_t *dst = buf(env, info_off, info_size < sizeof(aster_input_info_t)
				   ? info_size : sizeof(aster_input_info_t));
	if (!dst) {
		return ASTER_EINVAL;
	}
	aster_input_info_t in = {
		.size = sizeof(in), .version = 1,
		.kinds = (aster_plat_input_touch_available() ? ASTER_INPUT_TOUCH : 0) |
			 (aster_plat_input_keys_available() ? ASTER_INPUT_KEYS : 0),
		.max_points = aster_plat_input_touch_max_points(),
	};
	size_t n = info_size < sizeof(in) ? info_size : sizeof(in);
	memcpy(dst, &in, n);
	return ASTER_OK;
}

static int32_t n_input_touch(wasm_exec_env_t env, uint32_t pts_off, uint32_t max)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (max == 0) return 0;
	uint32_t available = aster_plat_input_touch_max_points();
	if (max > available) max = available;
	if (max == 0) return 0;
	uint8_t *dst = buf(env, pts_off, (size_t)max * sizeof(aster_touch_point_t));
	if (!dst) {
		return ASTER_EINVAL;
	}
	uint32_t tmp[ASTER_TOUCH_MAX * 4 + 4] = {0};
	aster_plat_input_state(tmp, available);
	/* Polled points arrive in panel space; the guest reads them against its
	 * own surface, which is app space. Mapping the single live point is the
	 * same call the event path makes, and "the finger is on the letterbox
	 * bars" is reported the same way a lifted finger is -- as an empty
	 * list, which is what an app polling for input can act on. */
	uint32_t written = 0;
	for (uint32_t src = 0; src < available && written < max; ++src) {
		if (tmp[src * 4 + 2] == ASTER_INPUT_TERM) break;
		aster_touch_point_t point = {
			.x = tmp[src * 4], .y = tmp[src * 4 + 1],
			.action = tmp[src * 4 + 2],
		};
		if (!aster_plat_display_app_touch(&point.x, &point.y)) continue;
		/* Guest offsets need not satisfy native struct alignment. */
		memcpy(dst + (size_t)written * sizeof(point), &point, sizeof(point));
		written++;
	}
	return (int32_t)written;
}

static int32_t n_input_keys_state(wasm_exec_env_t env, uint32_t codes_off, uint32_t max)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (max > ASTER_PLAT_MAX_KEYS) {
		max = ASTER_PLAT_MAX_KEYS;
	}
	uint32_t *dst = (uint32_t *)buf(env, codes_off, max * 4);
	if (!dst) {
		return ASTER_EINVAL;
	}
	uint32_t tmp[ASTER_PLAT_MAX_KEYS];
	int n = aster_plat_input_keys_state(tmp, (int)max);
	for (int i = 0; i < n; i++) {
		dst[i] = tmp[i];
	}
	return n;
}

/* ============ sensors (ABI 1.3, pull model) ============ */

/* One atomic fetch serves legacy channels and the coherent ABI 3.7 API.
 * The APP worker owns this cache; the platform serializes sensor transactions
 * against shell diagnostics. Retry errors after the same TTL, never publish
 * a stale or partially filled sample on a failed fetch. */
#define IMU_CACHE_MS 10
static struct {
	int64_t at_ms;
	aster_imu_sample_t sample;
	int32_t result;
} imu_cache = { .at_ms = -1 };

static int32_t imu_cached(void)
{
	int64_t now = k_uptime_get();

	if (imu_cache.at_ms >= 0 && now - imu_cache.at_ms < IMU_CACHE_MS) {
		return imu_cache.result;
	}
	aster_imu_sample_t next = {
		.size = sizeof(next), .version = 1,
		.flags = ASTER_IMU_ACCEL_VALID | ASTER_IMU_GYRO_VALID,
		.sequence = imu_cache.sample.sequence + 1u,
	};
	int err = aster_plat_imu_read(&next.accel[0], &next.accel[1], &next.accel[2],
				    &next.gyro[0], &next.gyro[1], &next.gyro[2]);

	imu_cache.at_ms = k_uptime_get();
	imu_cache.result = err == -ENODEV ? ASTER_ENODEV : err ? ASTER_EIO : ASTER_OK;
	if (!err) {
		next.timestamp_ms = (uint64_t)imu_cache.at_ms;
		imu_cache.sample = next;
	}
	return imu_cache.result;
}

static int32_t n_imu_get_info(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
	aster_imu_info_t *dst;

	if (size < sizeof(*dst) || !(dst = (aster_imu_info_t *)buf(env, off, sizeof(*dst)))) {
		return ASTER_EINVAL;
	}
	if (!aster_sysinfo_has_feature(ASTER_FEATURE_ACCELEROMETER) ||
	    !aster_sysinfo_has_feature(ASTER_FEATURE_GYROSCOPE)) {
		return ASTER_ENODEV;
	}
	int32_t result = imu_cached();

	if (result != ASTER_OK) {
		return result;
	}
	aster_imu_info_t info = {
		.size = sizeof(info), .version = 1,
		.flags = ASTER_IMU_ACCEL_VALID | ASTER_IMU_GYRO_VALID,
		.min_interval_ms = IMU_CACHE_MS,
	};
	memcpy(dst, &info, sizeof(info));
	return ASTER_OK;
}

static int32_t n_imu_sample_read(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
	aster_imu_sample_t *dst;

	if (size < sizeof(*dst) || !(dst = (aster_imu_sample_t *)buf(env, off, sizeof(*dst)))) {
		return ASTER_EINVAL;
	}
	if (!aster_sysinfo_has_feature(ASTER_FEATURE_ACCELEROMETER) ||
	    !aster_sysinfo_has_feature(ASTER_FEATURE_GYROSCOPE)) {
		return ASTER_ENODEV;
	}
	int32_t result = imu_cached();

	if (result == ASTER_OK) {
		memcpy(dst, &imu_cache.sample, sizeof(*dst));
	}
	return result;
}

static int32_t n_imu_read(wasm_exec_env_t env, uint32_t off, uint32_t size, int gyro)
{
	if (size < 3 * sizeof(float)) {
		return ASTER_EINVAL;
	}
	if (!aster_sysinfo_has_feature(gyro ? ASTER_FEATURE_GYROSCOPE : ASTER_FEATURE_ACCELEROMETER)) {
		return ASTER_ENODEV;
	}
	float *dst = (float *)buf(env, off, 3 * sizeof(float));
	if (!dst) {
		return ASTER_EINVAL;
	}
	int32_t result = imu_cached();
	if (result != ASTER_OK) {
		return result;
	}
	memcpy(dst, gyro ? imu_cache.sample.gyro : imu_cache.sample.accel, 3 * sizeof(float));
	return ASTER_OK;
}

static int32_t n_accel_read(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
	return n_imu_read(env, off, size, 0);
}

static int32_t n_gyro_read(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
	return n_imu_read(env, off, size, 1);
}

/* ============ camera (ABI 1.3, single-shot still) ============ */

static int32_t cam_gate(void)
{
	return aster_sysinfo_has_feature(ASTER_FEATURE_CAMERA) ? ASTER_OK : ASTER_ENODEV;
}

/* Capture geometry belongs to the board, so ask the port. This file is
 * board-independent, and it used to read ASTER_PLAT_CAM_W/H -- one board's
 * numbers sitting in the shared port header -- to size-check every app
 * buffer. A port with a different sensor window would therefore have
 * rejected every capture ASTER_EINVAL before the driver was entered, and
 * nothing in the log would have named the cause. */
static int32_t cam_geometry(uint32_t *w, uint32_t *h)
{
#if IS_ENABLED(CONFIG_ASTER_CAMERA)
	if (aster_plat_cam_info(w, h) == 0) {
		return ASTER_OK;
	}
#endif
	*w = 0;
	*h = 0;
	return ASTER_ENODEV; /* capability bit without a driver behind it */
}

static int32_t n_cam_info(wasm_exec_env_t env, uint32_t w_off, uint32_t h_off)
{
	int32_t e = cam_gate();
	if (e != ASTER_OK) {
		return e;
	}
	uint32_t *w = w_off ? (uint32_t *)buf(env, w_off, 4) : NULL;
	uint32_t *h = h_off ? (uint32_t *)buf(env, h_off, 4) : NULL;
	if ((w_off && !w) || (h_off && !h)) {
		return ASTER_EINVAL;
	}
	uint32_t cw, ch;

	e = cam_geometry(&cw, &ch);
	if (e != ASTER_OK) {
		return e;
	}
	if (w) {
		*w = cw;
	}
	if (h) {
		*h = ch;
	}
	return ASTER_OK;
}

static int32_t n_cam_read(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
	int32_t e = cam_gate();
	if (e != ASTER_OK) {
		return e;
	}
	uint32_t cw, ch;

	e = cam_geometry(&cw, &ch);
	if (e != ASTER_OK) {
		return e;
	}
	uint32_t need = cw * ch * 2; /* RGB565 */
	if (size < need) {
		return ASTER_EINVAL;
	}
	uint8_t *dst = buf(env, off, need);
	if (!dst) {
		return ASTER_EINVAL;
	}
#if IS_ENABLED(CONFIG_ASTER_CAMERA)
	/* Direct into the app's linear memory: the video buffer DMA pool is
	 * drained by memcpy in the port, so no second 150 KB host bounce. */
	int err = aster_plat_cam_grab_rgb565(dst, need);
	if (err == -ETIMEDOUT) {
		return ASTER_ETIMEDOUT;
	}
	return err ? ASTER_EIO : ASTER_OK;
#else
	return ASTER_ENODEV; /* capability bit lied: profile without the driver */
#endif
}

/* ============ audio (ABI 1.6 PCM, 1.7 volume/session, 1.8 MP3, 1.9 file) ============ */

static int32_t audio_out_gate(void)
{
	return aster_sysinfo_has_feature(ASTER_FEATURE_AUDIO_OUTPUT) ? ASTER_OK : ASTER_ENODEV;
}

static int32_t audio_in_gate(void)
{
	return aster_sysinfo_has_feature(ASTER_FEATURE_AUDIO_INPUT) ? ASTER_OK : ASTER_ENODEV;
}

#if IS_ENABLED(CONFIG_ASTER_AUDIO)
static int32_t map_audio_err(int err)
{
	if (err == 0) {
		return ASTER_OK;
	}
	if (err == -EINVAL) {
		return ASTER_EINVAL;
	}
	if (err == -ENODEV) {
		return ASTER_ENODEV;
	}
	if (err == -EBUSY) {
		return ASTER_EBUSY;
	}
	if (err == -EAGAIN) {
		return ASTER_ESTATE;
	}
	if (err == -ENOMEM) {
		return ASTER_ENOMEM;
	}
	if (err == -ENOENT) {
		return ASTER_ENOENT;
	}
	if (err == -ETIMEDOUT) {
		return ASTER_ETIMEDOUT;
	}
	return ASTER_EIO;
}
#endif

/* 2.0 removed n_audio_play with its import: playback goes through the
 * background session (start_file). aster_audio_record stays -- capture has no
 * streaming replacement.
 *
 * 3.2 restores n_audio_get_info, which 2.0 dropped on the grounds that the
 * format was fixed by constants. It is fixed, but per *board*, and an app is
 * compiled once and installed anywhere: the ASTER_AUDIO_* macros it used to
 * read were a number that is wrong on every board but one. Asking is the
 * only answer that survives a codec change. */
static int32_t audio_frame_bytes(uint32_t *frame)
{
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	uint32_t rate, ch, bits;

	if (aster_plat_audio_info(&rate, &ch, &bits) == 0) {
		*frame = ch * (bits / 8u);
		return ASTER_OK;
	}
#endif
	*frame = 0;
	return ASTER_ENODEV; /* capability bit without a driver behind it */
}

static int32_t n_audio_get_info(wasm_exec_env_t env, uint32_t off, uint32_t info_size)
{
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
	/* Same forward-compat shape as get_state below: size + version are the
	 * contract, so they are the floor, and a caller built against an older
	 * (shorter) struct gets the prefix it knows. */
	if (info_size < 8) {
		return ASTER_EINVAL;
	}
	size_t n = info_size < sizeof(aster_audio_info_t) ? info_size : sizeof(aster_audio_info_t);
	uint8_t *dst = buf(env, off, n);
	if (!dst) {
		return ASTER_EINVAL;
	}
	aster_audio_info_t info = {
		.size = sizeof(aster_audio_info_t),
		.version = 1,
	};
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	uint32_t rate, ch, bits;

	if (aster_plat_audio_info(&rate, &ch, &bits) != 0) {
		return ASTER_ENODEV;
	}
	info.sample_rate = rate;
	info.channels = ch;
	info.bits = bits;
	info.flags = 0;
#else
	return ASTER_ENODEV;
#endif
	memcpy(dst, &info, n);
	return ASTER_OK;
}

#include "aster_audio_stream_bridge.inc"

static int32_t n_audio_record(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
	int32_t e = audio_in_gate();
	uint32_t frame;

	if (e != ASTER_OK) {
		return e;
	}
	e = audio_frame_bytes(&frame);
	if (e != ASTER_OK) {
		return e;
	}
	if (size == 0 || (size % frame) != 0) {
		return ASTER_EINVAL;
	}
	uint8_t *dst = buf(env, off, size);
	if (!dst) {
		return ASTER_EINVAL;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_record(dst, size));
#else
	ARG_UNUSED(dst);
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_set_volume(wasm_exec_env_t env, int32_t percent)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_set_volume(percent));
#else
	ARG_UNUSED(percent);
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_get_volume(wasm_exec_env_t env)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return aster_plat_audio_get_volume();
#else
	return ASTER_ENODEV;
#endif
}

/* 2.0: n_audio_start removed — the 2 MiB in-memory copy route is superseded
 * by start_file streaming (`hw audio` plays via aster_plat_audio_start_path). */

static int32_t n_audio_start_file(wasm_exec_env_t env, uint32_t off, uint32_t len)
{
	int32_t e = audio_out_gate();
	aster_app_ctx_t *ctx;
	char apath[160];
	char real[256];

	if (e != ASTER_OK) {
		return e;
	}
	ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EINVAL;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	if (app_str(env, off, len, apath, sizeof(apath)) < 0) {
		return ASTER_EINVAL;
	}
	if (sandbox_map(ctx, apath, real, sizeof(real)) != 0) {
		return ASTER_EPERM;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_start_path(real));
#else
	ARG_UNUSED(real);
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_effect_load(wasm_exec_env_t env, uint32_t off, uint32_t len)
{
	int32_t e = audio_out_gate();
	aster_app_ctx_t *ctx;
	char apath[160];
	char real[256];

	if (e != ASTER_OK) {
		return e;
	}
	ctx = ctx_of(env);
	if (!ctx || !aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	if (app_str(env, off, len, apath, sizeof(apath)) < 0 ||
	    sandbox_map(ctx, apath, real, sizeof(real)) != 0) {
		return ASTER_EINVAL;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	int handle = aster_plat_audio_effect_load(real);
	return handle > 0 ? handle : map_audio_err(handle);
#else
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_effect_play(wasm_exec_env_t env, uint32_t handle)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_effect_play(handle));
#else
	return ASTER_ENODEV;
#endif
}

/* Copy versioned guest descriptors before entering the audio service. */
static int32_t n_audio_effect_play_ex(wasm_exec_env_t env, uint32_t handle,
                                     uint32_t off, uint32_t size)
{
 int32_t e=audio_out_gate();
 if(e!=ASTER_OK) return e;
 if(size!=sizeof(aster_audio_effect_play_desc_t)) return ASTER_EINVAL;
 const void *src=buf(env,off,size);
 if(!src) return ASTER_EINVAL;
 aster_audio_effect_play_desc_t d;memcpy(&d,src,sizeof(d));
 if(d.size!=size || d.version!=1) return ASTER_EINVAL;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
 return map_audio_err(aster_plat_audio_effect_play_ex(handle,&d));
#else
 ARG_UNUSED(handle);return ASTER_ENODEV;
#endif
}
static int32_t n_audio_effect_set_params(wasm_exec_env_t env, uint32_t handle,
                                        uint32_t off, uint32_t size)
{
 int32_t e=audio_out_gate();
 if(e!=ASTER_OK) return e;
 if(size!=sizeof(aster_audio_effect_params_t)) return ASTER_EINVAL;
 const void *src=buf(env,off,size);
 if(!src) return ASTER_EINVAL;
 aster_audio_effect_params_t p;memcpy(&p,src,sizeof(p));
 if(p.size!=size || p.version!=1) return ASTER_EINVAL;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
 return map_audio_err(aster_plat_audio_effect_set_params(handle,&p));
#else
 ARG_UNUSED(handle);return ASTER_ENODEV;
#endif
}
static int32_t n_audio_effect_stop(wasm_exec_env_t env,uint32_t handle,uint32_t ms)
{
 ARG_UNUSED(env);
 int32_t e=audio_out_gate();
 if(e!=ASTER_OK) return e;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
 return map_audio_err(aster_plat_audio_effect_stop(handle,ms));
#else
 ARG_UNUSED(handle);ARG_UNUSED(ms);return ASTER_ENODEV;
#endif
}

static int32_t n_audio_effect_unload(wasm_exec_env_t env, uint32_t handle)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_effect_unload(handle));
#else
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_pause(wasm_exec_env_t env)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_pause());
#else
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_resume(wasm_exec_env_t env)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_resume());
#else
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_stop(wasm_exec_env_t env)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_stop());
#else
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_get_state(wasm_exec_env_t env, uint32_t off, uint32_t st_size)
{
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
	if (st_size < 8) {
		return ASTER_EINVAL;
	}
	size_t n = st_size < sizeof(aster_audio_state_t) ? st_size : sizeof(aster_audio_state_t);
	uint8_t *dst = buf(env, off, n);
	if (!dst) {
		return ASTER_EINVAL;
	}
	aster_audio_state_t st = {
		.size = sizeof(aster_audio_state_t),
		.version = 2,
	};
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	(void)aster_plat_audio_get_state(&st.state, &st.bytes_total, &st.bytes_done, &st.volume);
	(void)aster_plat_audio_get_clock(&st.msec_done, &st.msec_total);
#else
	return ASTER_ENODEV;
#endif
	memcpy(dst, &st, n);
	return ASTER_OK;
}

static int32_t n_audio_seek(wasm_exec_env_t env, uint32_t msec)
{
	ARG_UNUSED(env);
	int32_t e = audio_out_gate();
	if (e != ASTER_OK) {
		return e;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	return map_audio_err(aster_plat_audio_seek(msec));
#else
	ARG_UNUSED(msec);
	return ASTER_ENODEV;
#endif
}

static int32_t n_audio_get_lyrics(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
	int32_t e = audio_out_gate();
	char *dst;

	if (e != ASTER_OK) {
		return e;
	}
	if (size == 0) {
		return ASTER_EINVAL;
	}
	dst = (char *)buf(env, off, size);
	if (!dst) {
		return ASTER_EINVAL;
	}
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	{
		int n = aster_plat_audio_get_lyrics(dst, size);

		if (n < 0) {
			return map_audio_err(n);
		}
		return (int32_t)n;
	}
#else
	ARG_UNUSED(dst);
	return ASTER_ENODEV;
#endif
}

/* ============ Wi-Fi station (ABI 3.5, permission "wifi") ============ */

#if IS_ENABLED(CONFIG_ASTER_WIFI)
static int32_t map_wifi_err(int err)
{
	if (!err) return ASTER_OK;
	if (err == -EINVAL) return ASTER_EINVAL;
	if (err == -ENODEV) return ASTER_ENODEV;
	if (err == -ENOTSUP) return ASTER_ENOTSUP;
	if (err == -EBUSY) return ASTER_EBUSY;
	if (err == -EAGAIN || err == -EINPROGRESS) return ASTER_EAGAIN;
	if (err == -ENOMEM) return ASTER_ENOMEM;
	if (err == -ENOENT) return ASTER_ENOENT;
	if (err == -ETIMEDOUT) return ASTER_ETIMEDOUT;
	return ASTER_EIO;
}

static int32_t wifi_gate(wasm_exec_env_t env)
{
	aster_app_ctx_t *ctx = ctx_of(env);

	if (!ctx) return ASTER_EBADF;
	if (!aster_ctx_perm(ctx, ASTER_PERM_WIFI)) return ASTER_EPERM;
	return aster_sysinfo_has_feature(ASTER_FEATURE_WIFI) ? ASTER_OK : ASTER_ENOTSUP;
}
#endif

static int32_t n_wifi_scan_start(wasm_exec_env_t env)
{
#if IS_ENABLED(CONFIG_ASTER_WIFI)
	int32_t gate = wifi_gate(env);
	return gate ? gate : map_wifi_err(aster_plat_wifi_scan_start());
#else
	ARG_UNUSED(env);
	return ASTER_ENOTSUP;
#endif
}

static int32_t n_wifi_scan_results(wasm_exec_env_t env, uint32_t out_off,
				   uint32_t capacity)
{
#if IS_ENABLED(CONFIG_ASTER_WIFI)
	int32_t gate = wifi_gate(env);
	aster_wifi_ap_t rows[ASTER_WIFI_AP_MAX];
	int count = 0;
	int err;
	uint64_t bytes;
	uint8_t *dst;

	if (gate) return gate;
	if (capacity == 0 || capacity > ASTER_WIFI_AP_MAX) return ASTER_EINVAL;
	bytes = (uint64_t)capacity * sizeof(aster_wifi_ap_info_t);
	if (bytes > UINT32_MAX) return ASTER_EINVAL;
	dst = buf(env, out_off, (uint32_t)bytes);
	if (!dst) return ASTER_EINVAL;
	err = aster_plat_wifi_scan_results(rows, (int)capacity, &count);
	if (err) return map_wifi_err(err);
	for (int i = 0; i < count; i++) {
		aster_wifi_ap_info_t ap = {
			.size = sizeof(ap),
			.version = 1,
			.rssi = rows[i].rssi,
			.channel = rows[i].channel,
			.security = aster_plat_wifi_security_class(rows[i].security),
		};
		memcpy(ap.ssid, rows[i].ssid, sizeof(ap.ssid));
		memcpy(dst + (size_t)i * sizeof(ap), &ap, sizeof(ap));
	}
	return count;
#else
	ARG_UNUSED(env); ARG_UNUSED(out_off); ARG_UNUSED(capacity);
	return ASTER_ENOTSUP;
#endif
}

static int32_t n_wifi_connect(wasm_exec_env_t env, uint32_t ssid_off,
			      uint32_t ssid_len, uint32_t password_off,
			      uint32_t password_len)
{
#if IS_ENABLED(CONFIG_ASTER_WIFI)
	int32_t gate = wifi_gate(env);
	char ssid[ASTER_WIFI_SSID_MAX + 1];
	char password[65] = {0};

	if (gate) return gate;
	if (ssid_len == 0 || ssid_len > ASTER_WIFI_SSID_MAX || password_len > 64 ||
	    app_str(env, ssid_off, ssid_len, ssid, sizeof(ssid)) < 0 ||
	    strlen(ssid) != ssid_len) {
		return ASTER_EINVAL;
	}
	if (password_len &&
	    (app_str(env, password_off, password_len, password,
		     sizeof(password)) < 0 || strlen(password) != password_len)) {
		return ASTER_EINVAL;
	}
	return map_wifi_err(aster_plat_wifi_connect_start(ssid,
							 password_len ? password : NULL));
#else
	ARG_UNUSED(env); ARG_UNUSED(ssid_off); ARG_UNUSED(ssid_len);
	ARG_UNUSED(password_off); ARG_UNUSED(password_len);
	return ASTER_ENOTSUP;
#endif
}

static int32_t n_wifi_disconnect(wasm_exec_env_t env)
{
#if IS_ENABLED(CONFIG_ASTER_WIFI)
	int32_t gate = wifi_gate(env);
	return gate ? gate : map_wifi_err(aster_plat_wifi_disconnect_start());
#else
	ARG_UNUSED(env);
	return ASTER_ENOTSUP;
#endif
}

static int32_t n_wifi_reconnect(wasm_exec_env_t env)
{
#if IS_ENABLED(CONFIG_ASTER_WIFI)
	int32_t gate = wifi_gate(env);
	return gate ? gate : map_wifi_err(aster_plat_wifi_reconnect_start());
#else
	ARG_UNUSED(env);
	return ASTER_ENOTSUP;
#endif
}

static int32_t n_wifi_forget(wasm_exec_env_t env)
{
#if IS_ENABLED(CONFIG_ASTER_WIFI)
	int32_t gate = wifi_gate(env);
	return gate ? gate : map_wifi_err(aster_plat_wifi_forget_start());
#else
	ARG_UNUSED(env);
	return ASTER_ENOTSUP;
#endif
}

static int32_t n_wifi_status(wasm_exec_env_t env, uint32_t out_off,
			     uint32_t out_size)
{
#if IS_ENABLED(CONFIG_ASTER_WIFI)
	const uint32_t min_size = offsetof(aster_wifi_info_t, last_result) +
				  sizeof(((aster_wifi_info_t *)0)->last_result);
	aster_wifi_status_t platform = {0};
	aster_wifi_info_t status = {0};
	uint32_t operation = ASTER_PLAT_WIFI_OP_NONE;
	uint32_t last_operation = ASTER_PLAT_WIFI_OP_NONE;
	int last_result = -ENOENT;
	int32_t gate = wifi_gate(env);
	uint8_t *dst;
	uint32_t copy_size;
	int err;

	if (gate) return gate;
	if (out_size < min_size) return ASTER_EINVAL;
	copy_size = MIN(out_size, (uint32_t)sizeof(status));
	dst = buf(env, out_off, copy_size);
	if (!dst) return ASTER_EINVAL;
	err = aster_plat_wifi_status(&platform);
	if (err) return map_wifi_err(err);
	aster_plat_wifi_operation(&operation, &last_operation, &last_result);
	status.size = sizeof(status);
	status.version = 1;
	status.state = aster_plat_wifi_state_class(platform.state);
	status.operation = operation;
	status.last_operation = last_operation;
	status.last_result = map_wifi_err(last_result);
	status.rssi = platform.rssi;
	status.security = aster_plat_wifi_security_class(platform.security);
	memcpy(status.ssid, platform.ssid, sizeof(status.ssid));
	memcpy(status.ipv4, platform.ip, sizeof(status.ipv4));
	memcpy(dst, &status, copy_size);
	return ASTER_OK;
#else
	ARG_UNUSED(env); ARG_UNUSED(out_off); ARG_UNUSED(out_size);
	return ASTER_ENOTSUP;
#endif
}

static int32_t n_wifi_get_saved_ssid(wasm_exec_env_t env, uint32_t out_off,
				     uint32_t capacity)
{
#if IS_ENABLED(CONFIG_ASTER_WIFI)
	int32_t gate = wifi_gate(env);
	uint8_t *dst;
	int err;

	if (gate) return gate;
	if (capacity < ASTER_WIFI_SSID_MAX + 1) return ASTER_EINVAL;
	dst = buf(env, out_off, ASTER_WIFI_SSID_MAX + 1);
	if (!dst) return ASTER_EINVAL;
	err = aster_plat_wifi_last_ssid((char *)dst, ASTER_WIFI_SSID_MAX + 1);
	return map_wifi_err(err);
#else
	ARG_UNUSED(env); ARG_UNUSED(out_off); ARG_UNUSED(capacity);
	return ASTER_ENOTSUP;
#endif
}

/* ============ fs ============ */

static int fs_slot(aster_app_ctx_t *ctx)
{
	for (int i = 0; i < ASTER_FILE_MAX; i++) {
		if (!ctx->files[i].used) {
			return i;
		}
	}
	return -1;
}

static int32_t n_fs_open(wasm_exec_env_t env, uint32_t path_off, uint32_t path_len,
			 uint32_t flags, uint32_t out_off)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	uint32_t *out = (uint32_t *)buf(env, out_off, 4);
	if (!ctx || !out) {
		return ASTER_EINVAL;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	char apath[160];
	if (app_str(env, path_off, path_len, apath, sizeof(apath)) < 0) {
		return ASTER_EINVAL;
	}
	char real[256];
	if (sandbox_map(ctx, apath, real, sizeof(real)) != 0) {
		return ASTER_EPERM;
	}
	if (aster_storage_is_music_path(apath) && flags != ASTER_FS_O_RDONLY) {
		return ASTER_EPERM;
	}
	int slot = fs_slot(ctx);
	if (slot < 0) {
		return ASTER_ENOMEM;
	}
	struct fs_file_t *f = k_malloc(sizeof(*f));
	if (!f) {
		return ASTER_ENOMEM;
	}
	fs_file_t_init(f);
	fs_mode_t zflags = 0;
	if ((flags & 0x03u) == ASTER_FS_O_RDONLY) {
		zflags = FS_O_READ;
	} else if ((flags & 0x03u) == ASTER_FS_O_WRONLY) {
		zflags = FS_O_WRITE;
	} else {
		zflags = FS_O_RDWR;
	}
	if (flags & ASTER_FS_O_CREAT) {
		zflags |= FS_O_CREATE;
	}
	if (flags & ASTER_FS_O_TRUNC) {
		zflags |= FS_O_TRUNC;
	}
	if (flags & ASTER_FS_O_APPEND) {
		zflags |= FS_O_APPEND;
	}
	/* ensure parent dir exists for /data writes */
	if (flags & ASTER_FS_O_CREAT) {
		char dir[256];
		strncpy(dir, real, sizeof(dir) - 1);
		dir[sizeof(dir) - 1] = 0;
		char *slash = strrchr(dir, '/');
		if (slash) {
			*slash = 0;
			aster_storage_mkdir_p(dir);
		}
	}
	int err = fs_open(f, real, zflags);
	if (err) {
		k_free(f);
		return err == -ENOENT ? ASTER_ENOENT : ASTER_EIO;
	}
	ctx->files[slot].f = f;
	ctx->files[slot].used = true;
	*out = ASTER_HTYPE_FILE | (uint32_t)(slot + 1);
	return ASTER_OK;
}

static int32_t n_fs_read(wasm_exec_env_t env, uint32_t h, uint32_t dst_off, uint32_t len)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	int i = handle_index(h, ASTER_HTYPE_FILE, ASTER_FILE_MAX);
	if (i < 0 || !ctx->files[i].used) {
		return ASTER_EBADF;
	}
	uint8_t *dst = buf(env, dst_off, len);
	if (!dst) {
		return ASTER_EINVAL;
	}
	/* dst lives in wasm linear memory (external RAM): must bounce for
	 * SDMMC DMA when that region is not DMA-capable on this platform */
	int r = aster_fs_read_safe(ctx->files[i].f, dst, len);
	return r < 0 ? ASTER_EIO : r;
}

static int32_t n_fs_write(wasm_exec_env_t env, uint32_t h, uint32_t src_off, uint32_t len)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	int i = handle_index(h, ASTER_HTYPE_FILE, ASTER_FILE_MAX);
	if (i < 0 || !ctx->files[i].used) {
		return ASTER_EBADF;
	}
	const uint8_t *src = buf(env, src_off, len);
	if (!src) {
		return ASTER_EINVAL;
	}
	int r = aster_fs_write_safe(ctx->files[i].f, src, len);
	return r < 0 ? ASTER_EIO : r;
}

static int32_t n_fs_seek(wasm_exec_env_t env, uint32_t h, int32_t ofs, uint32_t whence)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	int i = handle_index(h, ASTER_HTYPE_FILE, ASTER_FILE_MAX);
	if (i < 0 || !ctx->files[i].used) {
		return ASTER_EBADF;
	}
	int w = FS_SEEK_SET;
	if (whence == ASTER_FS_SEEK_CUR) {
		w = FS_SEEK_CUR;
	} else if (whence == ASTER_FS_SEEK_END) {
		w = FS_SEEK_END;
	}
	int r = fs_seek(ctx->files[i].f, ofs, w);
	return r < 0 ? ASTER_EIO : r;
}

static int32_t close_file(aster_app_ctx_t *ctx, int i)
{
	if (ctx->files[i].used) {
		fs_close(ctx->files[i].f);
		k_free(ctx->files[i].f);
		ctx->files[i].f = NULL;
		ctx->files[i].used = false;
	}
	return ASTER_OK;
}

static int32_t n_fs_close(wasm_exec_env_t env, uint32_t h)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	int i = handle_index(h, ASTER_HTYPE_FILE, ASTER_FILE_MAX);
	if (i < 0) {
		return ASTER_EBADF;
	}
	return close_file(ctx, i);
}

static int32_t n_fs_mkdir(wasm_exec_env_t env, uint32_t path_off, uint32_t path_len)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	char apath[160], real[256];
	if (app_str(env, path_off, path_len, apath, sizeof(apath)) < 0) {
		return ASTER_EINVAL;
	}
	if (sandbox_map(ctx, apath, real, sizeof(real)) != 0) {
		return ASTER_EPERM;
	}
	if (aster_storage_is_music_path(apath)) {
		return ASTER_EPERM;
	}
	int err = aster_storage_mkdir_p(real);
	return err ? ASTER_EIO : ASTER_OK;
}

static int32_t n_fs_unlink(wasm_exec_env_t env, uint32_t path_off, uint32_t path_len)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	char apath[160], real[256];
	if (app_str(env, path_off, path_len, apath, sizeof(apath)) < 0) {
		return ASTER_EINVAL;
	}
	if (sandbox_map(ctx, apath, real, sizeof(real)) != 0) {
		return ASTER_EPERM;
	}
	if (aster_storage_is_music_path(apath)) {
		return ASTER_EPERM;
	}
	int err = fs_unlink(real);
	return err == -ENOENT ? ASTER_ENOENT : (err ? ASTER_EIO : ASTER_OK);
}

static int32_t n_fs_stat(wasm_exec_env_t env, uint32_t path_off, uint32_t path_len,
			 uint32_t st_off, uint32_t st_size)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	uint8_t *dst = buf(env, st_off, st_size < sizeof(aster_stat_t) ? st_size
					       : sizeof(aster_stat_t));
	if (!ctx || !dst) {
		return ASTER_EINVAL;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	char apath[160], real[256];
	if (app_str(env, path_off, path_len, apath, sizeof(apath)) < 0) {
		return ASTER_EINVAL;
	}
	if (sandbox_map(ctx, apath, real, sizeof(real)) != 0) {
		return ASTER_EPERM;
	}
	struct fs_dirent ent;
	int err = fs_stat(real, &ent);
	if (err) {
		return ASTER_ENOENT;
	}
	aster_stat_t st = {
		.size = sizeof(st), .version = 1,
		.filesize = (uint64_t)ent.size,
		.mode = ent.type == FS_DIR_ENTRY_DIR ? ASTER_STAT_DIR : 0,
	};
	size_t n = st_size < sizeof(st) ? st_size : sizeof(st);
	memcpy(dst, &st, n);
	return ASTER_OK;
}

/* ---- dir cursors ----
 * Guest dir handles index this table (ASTER_HTYPE_DIR | (idx + 1)) and every
 * entry records the context that opened it, so a cursor is reachable only
 * through its owner: two contexts can never iterate over each other's
 * cursor, nor close it. The shell lists directories with its own
 * struct fs_dir_t on the stack and never touches these slots. Only the
 * owning app's worker thread runs here (native calls and ctx_cleanup), so
 * no lock is needed.
 * Cursors deliberately do not squat in ctx->files[]: a dir parked there
 * left a slot marked used with f == NULL, and a forged ASTER_HTYPE_FILE
 * handle for that index fed NULL to fs_close()/fs_seek().
 */
static struct {
	aster_app_ctx_t   *owner; /* NULL = free */
	struct fs_dir_t *d;
} dir_slots[ASTER_FILE_MAX];

static int dir_slot_alloc(aster_app_ctx_t *ctx, struct fs_dir_t *d)
{
	for (int i = 0; i < ASTER_FILE_MAX; i++) {
		if (!dir_slots[i].owner) {
			dir_slots[i].owner = ctx;
			dir_slots[i].d = d;
			return i;
		}
	}
	return -1;
}

static void dir_slot_clear(int slot)
{
	if (slot < 0 || slot >= ASTER_FILE_MAX) {
		return;
	}
	if (dir_slots[slot].d) {
		fs_closedir(dir_slots[slot].d);
		k_free(dir_slots[slot].d);
	}
	dir_slots[slot].owner = NULL;
	dir_slots[slot].d = NULL;
}

/* Resolve guest handle `h` to the cursor `ctx` opened, NULL if the handle is
 * not a dir cursor of this context. */
static struct fs_dir_t *dir_slot_get(aster_app_ctx_t *ctx, uint32_t h)
{
	int slot = handle_index(h, ASTER_HTYPE_DIR, ASTER_FILE_MAX);

	if (!ctx || slot < 0 || dir_slots[slot].owner != ctx) {
		return NULL;
	}
	return dir_slots[slot].d;
}

static int32_t n_fs_opendir(wasm_exec_env_t env, uint32_t path_off, uint32_t path_len,
			    uint32_t out_off)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	uint32_t *out = (uint32_t *)buf(env, out_off, 4);
	if (!ctx || !out) {
		return ASTER_EINVAL;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	char apath[160], real[256];
	if (app_str(env, path_off, path_len, apath, sizeof(apath)) < 0) {
		return ASTER_EINVAL;
	}
	if (sandbox_map(ctx, apath, real, sizeof(real)) != 0) {
		return ASTER_EPERM;
	}
	struct fs_dir_t *d = k_malloc(sizeof(*d));
	if (!d) {
		return ASTER_ENOMEM;
	}
	fs_dir_t_init(d);
	if (fs_opendir(d, real) != 0) {
		k_free(d);
		return ASTER_ENOENT;
	}
	int slot = dir_slot_alloc(ctx, d);
	if (slot < 0) {
		fs_closedir(d);
		k_free(d);
		return ASTER_ENOMEM;
	}
	*out = ASTER_HTYPE_DIR | (uint32_t)(slot + 1);
	return ASTER_OK;
}

static int32_t n_fs_dir_next(wasm_exec_env_t env, uint32_t h, uint32_t name_off, uint32_t name_max,
			     uint32_t is_dir_off)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	struct fs_dir_t *d = dir_slot_get(ctx, h);
	char *name = (char *)buf(env, name_off, name_max);
	if (!d || !name) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	struct fs_dirent ent;
	for (;;) {
		int err = fs_readdir(d, &ent);

		if (err || ent.name[0] == 0) {
			return 0; /* end */
		}
		if (aster_fs_name_valid(ent.name)) {
			break;
		}
		/* Not a name -- skip it (see aster_fs_name_valid). */
	}
	uint32_t nl = (uint32_t)strlen(ent.name);
	if (nl >= name_max) {
		nl = name_max - 1;
	}
	memcpy(name, ent.name, nl);
	name[nl] = 0;
	if (is_dir_off) {
		uint32_t *id = (uint32_t *)buf(env, is_dir_off, 4);
		if (id) {
			*id = ent.type == FS_DIR_ENTRY_DIR ? 1 : 0;
		}
	}
	return (int32_t)nl;
}

static int32_t n_fs_closedir(wasm_exec_env_t env, uint32_t h)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	int slot = handle_index(h, ASTER_HTYPE_DIR, ASTER_FILE_MAX);
	if (!ctx || slot < 0 || dir_slots[slot].owner != ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	dir_slot_clear(slot);
	return ASTER_OK;
}

/* ============ img (ABI 1.14, permission "fs") ============
 * Picture preview: firmware-streamed decode into the app's own RGB565
 * buffer. Path is sandbox-mapped exactly like the fs family; the decode
 * blocks the app worker (drive it from an event, not every frame). */

#if IS_ENABLED(CONFIG_ASTER_IMGDEC)

static int32_t n_img_probe(wasm_exec_env_t env, uint32_t path_off, uint32_t path_len,
			   uint32_t out_off, uint32_t out_size)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	/* ABI wire struct (matches abi/include/aster/img.h::aster_img_info_t). The
	 * firmware-internal aster_imgdec_info_t has a different layout, so copy
	 * field-by-field across the boundary. */
	typedef struct {
		uint32_t size, version, fmt, src_w, src_h, file_size, out_w, out_h;
		uint32_t flags, reserved;
		char     why[48];
	} img_info_wire_t;
	img_info_wire_t *dst = (img_info_wire_t *)buf(env, out_off,
		out_size < sizeof(img_info_wire_t) ? out_size : sizeof(img_info_wire_t));
	if (!dst) {
		return ASTER_EINVAL;
	}
	char apath[160];
	if (app_str(env, path_off, path_len, apath, sizeof(apath)) < 0) {
		return ASTER_EINVAL;
	}
	char real[256];
	if (sandbox_map(ctx, apath, real, sizeof(real)) != 0) {
		return ASTER_EPERM;
	}
	/* The cap is the guest's own surface, which is app space -- the panel
	 * turned portrait, the same in both orientations -- and not the panel,
	 * so "how big a picture can I show" has one answer per board. */
	uint32_t cw, ch;

	if (!aster_plat_display_app_space(&cw, &ch)) {
		return ASTER_ENOTSUP;
	}
	aster_imgdec_info_t info;
	aster_error_t rc = aster_imgdec_probe(real, &info, cw, ch);
	memset(dst, 0, sizeof(*dst));
	dst->size = sizeof(img_info_wire_t);
	dst->version = 1;
	dst->fmt = (uint32_t)info.fmt;
	dst->src_w = info.src_w;
	dst->src_h = info.src_h;
	dst->file_size = info.file_size;
	dst->out_w = info.out_w;
	dst->out_h = info.out_h;
	dst->flags = (info.unsupported ? 1u : 0u) | (info.too_big ? 2u : 0u);
	memcpy(dst->why, info.why, sizeof(dst->why));
	dst->why[sizeof(dst->why) - 1] = 0;
	return rc;
}

static int32_t n_img_render(wasm_exec_env_t env, uint32_t path_off, uint32_t path_len,
			    uint32_t args_off, uint32_t args_size)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_FS)) {
		return ASTER_EPERM;
	}
	aster_img_render_args_t *a = (aster_img_render_args_t *)buf(env, args_off,
		args_size < sizeof(aster_img_render_args_t) ? args_size
						     : sizeof(aster_img_render_args_t));
	if (!a || a->size < sizeof(*a)) {
		return ASTER_EINVAL;
	}
	char apath[160];
	if (app_str(env, path_off, path_len, apath, sizeof(apath)) < 0) {
		return ASTER_EINVAL;
	}
	char real[256];
	if (sandbox_map(ctx, apath, real, sizeof(real)) != 0) {
		return ASTER_EPERM;
	}
	if (!a->cap_w || !a->cap_h) {
		return ASTER_EINVAL;
	}
	/* The decoder writes at most cap_w*cap_h*2 packed bytes (fit never
	 * upscales, out_w <= cap_w). Validate the FULL cap region against the
	 * app's linear memory -- not just dst_span -- so a lying span cannot
	 * make the host scribble past the app's real buffer. A span larger
	 * than the region is clamped; a smaller one fails validation. */
	uint64_t need = (uint64_t)a->cap_w * a->cap_h * 2u;
	if (need >= UINT32_MAX) {
		return ASTER_EINVAL;
	}
	if (a->dst_span == 0 || a->dst_span > (uint32_t)need) {
		a->dst_span = (uint32_t)need;
	}
	uint8_t *dst = buf(env, a->dst_off, (uint32_t)need);
	if (!dst) {
		return ASTER_EINVAL;
	}
	uint32_t ow = 0, oh = 0;
	aster_error_t rc = aster_imgdec_render(real, dst, a->cap_w, a->cap_h, &ow, &oh,
					(a->flags & ASTER_IMG_F_NEAREST) != 0);
	a->out_w = ow;
	a->out_h = oh;
	return rc;
}

#else /* !CONFIG_ASTER_IMGDEC: symbols stay registered, refuse at runtime */

static int32_t n_img_probe(wasm_exec_env_t env, uint32_t path_off, uint32_t path_len,
			   uint32_t out_off, uint32_t out_size)
{
	ARG_UNUSED(env); ARG_UNUSED(path_off); ARG_UNUSED(path_len);
	ARG_UNUSED(out_off); ARG_UNUSED(out_size);
	return ASTER_ENOTSUP;
}

static int32_t n_img_render(wasm_exec_env_t env, uint32_t path_off, uint32_t path_len,
			    uint32_t args_off, uint32_t args_size)
{
	ARG_UNUSED(env); ARG_UNUSED(path_off); ARG_UNUSED(path_len);
	ARG_UNUSED(args_off); ARG_UNUSED(args_size);
	return ASTER_ENOTSUP;
}

#endif /* CONFIG_ASTER_IMGDEC */

/* ============ kv ============ */

static int32_t n_kv_set(wasm_exec_env_t env, uint32_t key_off, uint32_t key_len,
			uint32_t val_off, uint32_t val_len)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_KV)) {
		return ASTER_EPERM;
	}
	char key[64];
	const uint8_t *val = buf(env, val_off, val_len);
	if (app_str(env, key_off, key_len, key, sizeof(key)) < 0 || !val) {
		return ASTER_EINVAL;
	}
	return aster_kv_set(ctx->id, key, val, val_len) ? ASTER_EIO : ASTER_OK;
}

static int32_t n_kv_get(wasm_exec_env_t env, uint32_t key_off, uint32_t key_len,
			uint32_t dst_off, uint32_t dst_max)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_KV)) {
		return ASTER_EPERM;
	}
	char key[64];
	uint8_t *dst = buf(env, dst_off, dst_max);
	if (app_str(env, key_off, key_len, key, sizeof(key)) < 0 || !dst) {
		return ASTER_EINVAL;
	}
	size_t out = 0;
	int err = aster_kv_get(ctx->id, key, dst, dst_max, &out);
	return err ? ASTER_ENOENT : (int32_t)out;
}

static int32_t n_kv_delete(wasm_exec_env_t env, uint32_t key_off, uint32_t key_len)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!ctx) {
		return ASTER_EBADF;
	}
	if (!aster_ctx_perm(ctx, ASTER_PERM_KV)) {
		return ASTER_EPERM;
	}
	char key[64];
	if (app_str(env, key_off, key_len, key, sizeof(key)) < 0) {
		return ASTER_EINVAL;
	}
	return aster_kv_delete(ctx->id, key) ? ASTER_EIO : ASTER_OK;
}

/* ============ appmgr (ABI 1.2, permission "appmgr") ============ */

static int32_t n_appmgr_count(wasm_exec_env_t env)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!aster_ctx_perm(ctx, ASTER_PERM_APPMGR)) {
		return ASTER_EPERM;
	}
	return aster_app_mgr_count();
}

static int32_t n_appmgr_list(wasm_exec_env_t env, uint32_t index, uint32_t info_off,
			     uint32_t info_size)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!aster_ctx_perm(ctx, ASTER_PERM_APPMGR)) {
		return ASTER_EPERM;
	}
	const aster_app_rec_t *r = aster_app_mgr_get((int)index);
	if (!r) {
		return ASTER_EINVAL;
	}
	uint8_t *dst = buf(env, info_off, info_size < sizeof(aster_app_info_t)
				   ? info_size : sizeof(aster_app_info_t));
	if (!dst) {
		return ASTER_EINVAL;
	}
	aster_app_info_t ai = { 0 };
	ai.size = sizeof(ai);
	ai.version = 2;
	/* snprintf, not strncpy: the record strings are the same width as the
	 * ABI fields, so strncpy's bound is exactly the source length and GCC
	 * rightly warns it may not terminate. snprintf always does. */
	snprintf(ai.id, sizeof(ai.id), "%s", r->id);
	snprintf(ai.name, sizeof(ai.name), "%s", r->name);
	snprintf(ai.app_version, sizeof(ai.app_version), "%s", r->version);
	ai.state = (uint32_t)r->state;
	ai.has_aot = r->has_aot ? 1u : 0u;
	snprintf(ai.category, sizeof(ai.category), "%s", r->category);
	ai.has_icon = r->has_icon ? 1u : 0u;
	size_t n = info_size < sizeof(ai) ? info_size : sizeof(ai);
	memcpy(dst, &ai, n);
	return ASTER_OK;
}

static int32_t n_appmgr_start(wasm_exec_env_t env, uint32_t id_off, uint32_t id_len)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!aster_ctx_perm(ctx, ASTER_PERM_APPMGR)) {
		return ASTER_EPERM;
	}
	char id[64];
	if (app_str(env, id_off, id_len, id, sizeof(id)) < 0) {
		return ASTER_EINVAL;
	}
	return aster_runtime_handoff(id);
}

static int32_t n_appmgr_uninstall(wasm_exec_env_t env, uint32_t id_off, uint32_t id_len)
{
	aster_app_ctx_t *ctx = ctx_of(env);
	if (!aster_ctx_perm(ctx, ASTER_PERM_APPMGR)) {
		return ASTER_EPERM;
	}
	char id[64];
	if (app_str(env, id_off, id_len, id, sizeof(id)) < 0) {
		return ASTER_EINVAL;
	}
	if (!strcmp(id, ctx->id)) {
		return ASTER_EINVAL; /* cannot uninstall self */
	}
	const aster_app_rec_t *r = aster_app_mgr_find(id);
	if (!r) {
		return ASTER_ENOENT;
	}
	if (r->state == ASTER_APP_STATE_RUNNING || r->state == ASTER_APP_STATE_PAUSED) {
		return ASTER_EBUSY;
	}
	return aster_app_mgr_uninstall(id);
}

/* Release any host fs handles still open by a stopping app. */
void aster_native_release_files(aster_app_ctx_t *ctx)
{
	if (!ctx) {
		return;
	}
	for (int i = 0; i < ASTER_FILE_MAX; i++) {
		/* cursors belong to the ctx, not to a file slot */
		if (dir_slots[i].owner == ctx) {
			dir_slot_clear(i);
		}
		if (ctx->files[i].used) {
			if (ctx->files[i].f) {
				fs_close(ctx->files[i].f);
				k_free(ctx->files[i].f);
				ctx->files[i].f = NULL;
			}
			ctx->files[i].used = false;
		}
	}
}

/* ============ libc-builtin subset (math/str) for C apps ============ */

/* memcpy for clang-emitted struct-copy calls (LVGL and any large-C app).
 * Offsets are app-linear-memory addresses, validated via buf(). Returns dest
 * like the C ABI (wasm import type (i32,i32,i32)->i32). */
static uint32_t n_memcpy(wasm_exec_env_t env, uint32_t dest, uint32_t src,
			 uint32_t n)
{
	if (n == 0) {
		return dest;
	}
	uint8_t *d = buf(env, dest, n);
	uint8_t *s = buf(env, src, n);
	if (!d || !s) {
		return 0;
	}
	/* both live in the same linear-memory block -> host memcpy is safe
	 * and word-fast (LVGL draw loops are memcpy-heavy) */
	memcpy(d, s, n);
	return dest;
}

/* symbol table (sorted by name: WAMR uses it as-is with linear search) */
static NativeSymbol syms[] = {
	{ "memcpy", (void *)n_memcpy, "(iii)i", NULL },
	{ "aster_app_exit", (void *)n_app_exit, "(i)", NULL },
	{ "aster_app_set_framerate", (void *)n_set_framerate, "(i)i", NULL },
	{ "aster_appmgr_count", (void *)n_appmgr_count, "()i", NULL },
	{ "aster_appmgr_list", (void *)n_appmgr_list, "(iii)i", NULL },
	{ "aster_appmgr_start", (void *)n_appmgr_start, "(ii)i", NULL },
	{ "aster_appmgr_uninstall", (void *)n_appmgr_uninstall, "(ii)i", NULL },
	{ "aster_audio_get_info", (void *)n_audio_get_info, "(ii)i", NULL },
	{ "aster_audio_get_lyrics", (void *)n_audio_get_lyrics, "(ii)i", NULL },
	{ "aster_audio_get_state", (void *)n_audio_get_state, "(ii)i", NULL },
	{ "aster_audio_get_volume", (void *)n_audio_get_volume, "()i", NULL },
	{ "aster_audio_effect_load", (void *)n_audio_effect_load, "(ii)i", NULL },
	{ "aster_audio_effect_play", (void *)n_audio_effect_play, "(i)i", NULL },
	{ "aster_audio_effect_play_ex", (void *)n_audio_effect_play_ex, "(iii)i", NULL },
	{ "aster_audio_effect_set_params", (void *)n_audio_effect_set_params, "(iii)i", NULL },
	{ "aster_audio_effect_stop", (void *)n_audio_effect_stop, "(ii)i", NULL },
	{ "aster_audio_effect_unload", (void *)n_audio_effect_unload, "(i)i", NULL },
	{ "aster_audio_pause", (void *)n_audio_pause, "()i", NULL },
	{ "aster_audio_record", (void *)n_audio_record, "(ii)i", NULL },
	{ "aster_audio_resume", (void *)n_audio_resume, "()i", NULL },
	{ "aster_audio_seek", (void *)n_audio_seek, "(i)i", NULL },
	{ "aster_audio_set_volume", (void *)n_audio_set_volume, "(i)i", NULL },
	{ "aster_audio_start_file", (void *)n_audio_start_file, "(ii)i", NULL },
	{ "aster_audio_stream_open", (void *)n_audio_stream_open, "(i)i", NULL },
	{ "aster_audio_stream_write", (void *)n_audio_stream_write, "(iii)i", NULL },
	{ "aster_audio_stream_finish", (void *)n_audio_stream_finish, "(i)i", NULL },
	{ "aster_audio_stream_status", (void *)n_audio_stream_status, "(iii)i", NULL },
	{ "aster_audio_stream_close", (void *)n_audio_stream_close, "(i)i", NULL },
	{ "aster_audio_stop", (void *)n_audio_stop, "()i", NULL },
	{ "aster_display_count", (void *)n_display_count, "()i", NULL },
	{ "aster_display_get_info", (void *)n_display_get_info, "(iii)i", NULL },
	{ "aster_fs_closedir", (void *)n_fs_closedir, "(i)i", NULL },
	{ "aster_fs_close", (void *)n_fs_close, "(i)i", NULL },
	{ "aster_fs_dir_next", (void *)n_fs_dir_next, "(iiii)i", NULL },
	{ "aster_fs_mkdir", (void *)n_fs_mkdir, "(ii)i", NULL },
	{ "aster_fs_open", (void *)n_fs_open, "(iiii)i", NULL },
	{ "aster_fs_opendir", (void *)n_fs_opendir, "(iii)i", NULL },
	{ "aster_fs_read", (void *)n_fs_read, "(iii)i", NULL },
	{ "aster_fs_seek", (void *)n_fs_seek, "(iii)i", NULL },
	{ "aster_fs_stat", (void *)n_fs_stat, "(iiii)i", NULL },
	{ "aster_fs_unlink", (void *)n_fs_unlink, "(ii)i", NULL },
	{ "aster_fs_write", (void *)n_fs_write, "(iii)i", NULL },
	{ "aster_img_probe", (void *)n_img_probe, "(iiii)i", NULL },
	{ "aster_img_render", (void *)n_img_render, "(iiii)i", NULL },
	{ "aster_input_get_info", (void *)n_input_get_info, "(ii)i", NULL },
	{ "aster_input_keys_state", (void *)n_input_keys_state, "(ii)i", NULL },
	{ "aster_imu_get_info", (void *)n_imu_get_info, "(ii)i", NULL },
	{ "aster_imu_read", (void *)n_imu_sample_read, "(ii)i", NULL },
	{ "aster_accel_read", (void *)n_accel_read, "(ii)i", NULL },
	{ "aster_gyro_read", (void *)n_gyro_read, "(ii)i", NULL },
	{ "aster_cam_info", (void *)n_cam_info, "(ii)i", NULL },
	{ "aster_cam_read", (void *)n_cam_read, "(ii)i", NULL },
	{ "aster_input_touch", (void *)n_input_touch, "(ii)i", NULL },
	{ "aster_kv_delete", (void *)n_kv_delete, "(ii)i", NULL },
	{ "aster_kv_get", (void *)n_kv_get, "(iiii)i", NULL },
	{ "aster_kv_set", (void *)n_kv_set, "(iiii)i", NULL },
	{ "aster_log", (void *)n_log, "(iii)", NULL },
	{ "aster_surface_acquire", (void *)n_surface_acquire, "(iii)i", NULL },
	{ "aster_surface_create", (void *)n_surface_create, "(iii)i", NULL },
	{ "aster_surface_destroy", (void *)n_surface_destroy, "(i)i", NULL },
	{ "aster_surface_flush", (void *)n_surface_flush, "(iiiiiii)i", NULL },
	{ "aster_surface_present", (void *)n_surface_present, "(i)i", NULL },
	{ "aster_gfx_exec", (void *)n_gfx_exec, "(iiiiiiiii)i", NULL },
	{ "aster_gfx_text_w", (void *)n_gfx_text_w, "(iii)i", NULL },
	{ "aster_gfx_fonts_pending", (void *)n_gfx_fonts_pending, "()i", NULL },
	{ "aster_gfx_bmp_load", (void *)n_gfx_bmp_load, "(iiiiiii)i", NULL },
	{ "aster_gfx_bmp_free", (void *)n_gfx_bmp_free, "(i)i", NULL },
	{ "aster_system_get_abi_version", (void *)n_system_get_abi, "()i", NULL },
	{ "aster_system_has_feature", (void *)n_system_has_feature, "(i)i", NULL },
	{ "aster_time_get", (void *)n_time_get, "(ii)i", NULL },
	{ "aster_time_now_ms", (void *)n_time_now_ms, "()i", NULL },
	{ "aster_timer_create", (void *)n_timer_create, "(iiii)i", NULL },
	{ "aster_timer_destroy", (void *)n_timer_destroy, "(i)i", NULL },
	{ "aster_wifi_connect", (void *)n_wifi_connect, "(iiii)i", NULL },
	{ "aster_wifi_disconnect", (void *)n_wifi_disconnect, "()i", NULL },
	{ "aster_wifi_forget", (void *)n_wifi_forget, "()i", NULL },
	{ "aster_wifi_get_saved_ssid", (void *)n_wifi_get_saved_ssid, "(ii)i", NULL },
	{ "aster_wifi_reconnect", (void *)n_wifi_reconnect, "()i", NULL },
	{ "aster_wifi_scan_results", (void *)n_wifi_scan_results, "(ii)i", NULL },
	{ "aster_net_info", (void *)n_net_info, "(ii)i", NULL },
	{ "aster_net_open", (void *)n_net_open, "(ii)i", NULL },
	{ "aster_net_io", (void *)n_net_io, "(iiii)i", NULL },
	{ "aster_net_status", (void *)n_net_status, "(iii)i", NULL },
	{ "aster_net_cancel", (void *)n_net_cancel, "(i)i", NULL },
	{ "aster_net_close", (void *)n_net_close, "(i)i", NULL },
	{ "aster_wifi_scan_start", (void *)n_wifi_scan_start, "()i", NULL },
	{ "aster_wifi_status", (void *)n_wifi_status, "(ii)i", NULL },
};

NativeSymbol *aster_native_symbols(void)
{
	return syms;
}

uint32_t aster_native_symbol_count(void)
{
	return (uint32_t)(sizeof(syms) / sizeof(syms[0]));
}
