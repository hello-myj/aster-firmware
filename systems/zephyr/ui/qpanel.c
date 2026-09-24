#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * Quick-settings sheet. See qpanel.h for the contract: what it may ask of
 * the screen under it, why it is torn down by the same call that rebuilds
 * that screen, and which parts are the caller's problem.
 *
 * The sheet lives on the ACTIVE SCREEN, as its last two children (scrim,
 * then sheet) rather than on lv_layer_top(). Both work for hit-testing and
 * draw order, and BOTH show up in `aster ui screenshot` (that reads the
 * glass record first, and LVGL's flush writes the same one). The reason is
 * teardown ownership: lv_obj_clean() deletes only a node's own children and
 * does NOT touch lv_layer_top(), so a top-layer sheet would outlive the
 * host rebuilding its tree and linger as a ghost over a screen that no
 * longer exists. On the screen it dies with the screen. The cost is that
 * lv_obj_clean() on the screen deletes the sheet too, down a path no
 * qpanel entry point sees; the LV_EVENT_DELETE hook below is what makes
 * that safe rather than a dangling-pointer bug.
 *
 * Rows are laid out in a flex column, each an explicit height in whole
 * line boxes (ASTER_FONT_UI_LINE_H) plus air, so a re-bake that changes the
 * face's line box moves the sheet instead of clipping it.
 */
#include "qpanel.h"

#include "font_sd.h"
#include "ui_font.h"
#include "ui_geom.h"
#include "ui_lvgl.h"
#include "ui_wifi.h"

#include "aster_kv.h"
#include "aster_mem.h"
#include "aster_platform.h"
#include "aster_storage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aster_qpanel, LOG_LEVEL_INF);

/* ---------- geometry (240x320 logical) ---------- */
#define QP_PAD_X   12
#define QP_TITLE_H 34
#define QP_ROW_H   44 /* one line box + 9 px of air above and below */
#define QP_WIFI_H  70 /* two line boxes + the same air             */
#define QP_RULE     2 /* accent terminator under the last row      */
#define QP_BTN_W   64
#define QP_BTN_H   28
#define QP_BTN_GAP 8
/* The orientation chip, in the title row beside 关闭. Two glyphs wide, the
 * same as 关闭 -- it is a control, not a value, so it is sized like one. */
#define QP_ORIENT_W 56
/* the first line box of any row, and the second when there is one */
#define QP_ROW_Y   ((QP_ROW_H - ASTER_FONT_UI_LINE_H) / 2)
#define QP_WIFI_Y1 ((QP_WIFI_H - 2 * ASTER_FONT_UI_LINE_H) / 2)
#define QP_WIFI_Y2 (QP_WIFI_Y1 + ASTER_FONT_UI_LINE_H)
/* dot column: the dot is centred on the line box it belongs to */
#define QP_DOT_X   QP_PAD_X
#define QP_TXT_X   (QP_PAD_X + 16)
#define QP_DOT_DY  10 /* (ASTER_FONT_UI_LINE_H - 6) / 2 == 10 */
/* the theme row: a fixed "主题" and the current name beside it, both on
 * the row's single line box */
#define QP_THEME_X (QP_TXT_X + 48)

#define QP_VOL_LBL_W 40
#define QP_VOL_VAL_W 40
#define QP_VOL_GAP    8
#define QP_VOL_SL_X  (QP_PAD_X + QP_VOL_LBL_W + QP_VOL_GAP)

/* The rows are whole line boxes plus air, and the sheet is the sum of its
 * rows plus the accent rule. Both are load-bearing: a face re-bake that
 * changes ASTER_FONT_UI_LINE_H has to break the build here, the same way the
 * drawer tiles do, rather than clip the bottom row's descenders. */
BUILD_ASSERT(QP_TITLE_H >= ASTER_FONT_UI_LINE_H &&
	     QP_ROW_H >= ASTER_FONT_UI_LINE_H + 8 &&
	     QP_WIFI_H >= 2 * ASTER_FONT_UI_LINE_H + 8,
	     "quick-settings rows no longer fit the face's line box");

/* The sheet is the sum of its rows and the scrim is the screen, so a sheet
 * taller than the screen is a row the user cannot reach: lv_obj_set_size()
 * takes the height happily, the sheet is simply clipped, and nothing says
 * so. `h` is summed at runtime from these same terms -- this is that sum,
 * assembled for the worst case (every optional row this build can show) and
 * checked at compile time instead.
 *
 * 238 of 240 is the landscape figure: two pixels of margin. That is the
 * point of the assert, not a reason to relax it -- a face re-bake that grows
 * ASTER_FONT_UI_LINE_H takes it negative here rather than silently eating the
 * SD row off the bottom of the glass. */
#define QP_SHEET_MAX_H (QP_TITLE_H + QP_ROW_H /* SD */ + QP_RULE + \
			(IS_ENABLED(CONFIG_ASTER_AUDIO) ? QP_ROW_H : 0) + \
			(IS_ENABLED(CONFIG_ASTER_WIFI) ? QP_WIFI_H : 0) + \
			QP_ROW_H /* theme */)

BUILD_ASSERT(QP_SHEET_MAX_H <= ASTER_UI_H,
	     "quick-settings sheet is taller than the logical screen: its last "
	     "row would be clipped with no error (shrink QP_ROW_H/QP_TITLE_H)");

#if IS_ENABLED(CONFIG_ASTER_AUDIO)
#define QPANEL_KV_ID  "aster.qpanel"
#define QPANEL_KV_VOL "volume"
#endif

/* ---------- state (UI thread, except s_wifi_busy) ---------- */
static const aster_qpanel_theme_ops_t *s_ops;
static aster_qpanel_palette_t s_pal;

static lv_obj_t *s_scrim, *s_sheet;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
static lv_obj_t *s_vol_slider, *s_vol_val;
#endif
#if IS_ENABLED(CONFIG_ASTER_WIFI)
static lv_obj_t *s_dot_wifi, *s_wifi_lbl, *s_wifi_sub;
static lv_obj_t *s_wifi_btn, *s_wifi_btn_lbl;
/* Read on the UI thread, written by the Wi-Fi worker. */
static atomic_t s_wifi_busy = ATOMIC_INIT(0);
#endif
static lv_obj_t *s_theme_name, *s_theme_btn;
static lv_obj_t *s_dot_sd, *s_sd_lbl;
static lv_obj_t *s_orient_btn, *s_orient_lbl;
/* Whether the sheet was opened over the launcher. The orientation chip is
 * built only there: an app's orientation is a fact of its manifest, honoured
 * before it starts (runtime/S6), and turning the panel under a running app
 * swaps the coordinate space its surface was acquired in. */
static bool s_over_launcher;

/* What the sheet is covering, when what it is covering is an APP. NULL until
 * the first in-app open; the 150 KB is only ever asked for by a context that
 * can actually use it. */
static uint8_t *s_snap;
/* The guest is not allowed on the glass while this is set. It is exactly
 * "a sheet is up over an app and s_snap is the truth", so it is armed with
 * the sheet and cleared with it -- never left behind for the next app. */
static bool s_frozen;

/* ---------- small helpers ---------- */
static lv_color_t hexc(uint32_t c)
{
	return lv_color_hex(c);
}

static void no_scroll(lv_obj_t *o)
{
	lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static const char *theme_name(int i)
{
	if (s_ops && s_ops->name) {
		const char *n = s_ops->name(i);

		if (n && n[0]) {
			return n;
		}
	}
	return "主题";
}

/* Fill s_pal for the theme in use RIGHT NOW -- read on every build, not
 * captured at bind time: the theme can change while the sheet is closed. */
static void load_palette(void)
{
	/* The launcher's first theme (dark), as the fallback. The sheet has
	 * to be legible with no host bound at all -- which is the state an
	 * app context lands in, and the state a build with the native
	 * launcher disabled is in permanently. */
	aster_qpanel_palette_t p = {
		.bg = 0x243044,
		.fg = 0xE6EBF5,
		.dim = 0x8B95A8,
		.surface = 0x0A0E1A,
		.accent = 0x06B6D4,
	};

	if (s_ops && s_ops->palette) {
		s_ops->palette(&p);
	}
	s_pal = p;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, uint32_t color,
			  int align)
{
	lv_obj_t *o = lv_label_create(parent);

	lv_label_set_text(o, text);
	lv_obj_set_style_text_color(o, hexc(color), 0);
	lv_obj_set_style_text_font(o, aster_ui_font(ASTER_FONT_UI_PX), 0);
	aster_ui_font_warm(ASTER_FONT_UI_PX, text);
	lv_obj_set_style_text_align(o,
		align == 1 ? LV_TEXT_ALIGN_CENTER :
		align == 2 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
	return o;
}

static void set_label(lv_obj_t *o, const char *s)
{
	const char *cur;

	if (!o) {
		return;
	}
	cur = lv_label_get_text(o);
	if (cur && strcmp(cur, s) == 0) {
		return;
	}
	lv_label_set_text(o, s);
}

static void dot_color(lv_obj_t *d, uint32_t c)
{
	if (d) {
		lv_obj_set_style_bg_color(d, hexc(c), 0);
	}
}

static lv_obj_t *mk_dot(lv_obj_t *parent, int x, int y)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_set_size(o, 6, 6);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_style_bg_color(o, hexc(s_pal.dim), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, 3, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	/* A plain lv_obj is clickable by default (core/lv_obj.c sets
	 * LV_OBJ_FLAG_CLICKABLE in the constructor), so an untouched 6x6 dot
	 * would swallow presses meant for the row it sits in -- and, being
	 * the topmost thing at those coordinates, would make the row feel
	 * dead. */
	lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
	no_scroll(o);
	return o;
}

static lv_obj_t *mk_row(lv_obj_t *parent, int h, bool rule)
{
	lv_obj_t *r = lv_obj_create(parent);

	lv_obj_set_size(r, LV_PCT(100), h);
	lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(r, 0, 0);
	lv_obj_set_style_pad_all(r, 0, 0);
	lv_obj_set_style_border_width(r, rule ? 1 : 0, 0);
	lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(r, hexc(s_pal.dim), 0);
	lv_obj_set_style_border_opa(r, LV_OPA_30, 0);
	no_scroll(r);
	/* Left clickable on purpose: it absorbs presses that land on the row
	 * but not on a control, so they never reach the scrim below (which
	 * would close the sheet on a tap that clearly meant "inside"). */
	return r;
}

static lv_obj_t *mk_button(lv_obj_t *parent, int x, int y, int w, int h,
			   const char *text, lv_event_cb_t cb, lv_obj_t **out_lbl)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l;

	lv_obj_set_size(b, w, h);
	lv_obj_set_pos(b, x, y);
	lv_obj_set_style_bg_color(b, hexc(s_pal.surface), 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	/* The theme's own pressed tint, so the feedback follows the palette
	 * rather than a hardcoded "make it brighter". */
	lv_obj_set_style_bg_color(b, hexc(s_pal.dim), LV_STATE_PRESSED);
	lv_obj_set_style_radius(b, 8, 0);
	lv_obj_set_style_border_width(b, 0, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_pad_all(b, 0, 0);
	if (cb) {
		lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	}
	l = mk_label(b, text, s_pal.fg, 1);
	lv_obj_center(l);
	if (out_lbl) {
		*out_lbl = l;
	}
	return b;
}

/* ---------- teardown ---------- */
/* The one writer of the widget pointers, called both by the close path and
 * by LV_EVENT_DELETE. That second caller is the point: lv_obj_clean() on
 * the active screen (the launcher's rebuild) deletes this subtree without
 * going through any qpanel entry point, and a stale s_sheet would then be
 * dereferenced by the next tick. Registered on both roots, and idempotent,
 * so the order they die in does not matter. */
static bool forget(void)
{
	bool had = s_sheet != NULL;

	s_scrim = s_sheet = NULL;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	s_vol_slider = s_vol_val = NULL;
#endif
#if IS_ENABLED(CONFIG_ASTER_WIFI)
	s_dot_wifi = s_wifi_lbl = s_wifi_sub = NULL;
	s_wifi_btn = s_wifi_btn_lbl = NULL;
#endif
	s_theme_name = s_theme_btn = NULL;
	s_dot_sd = s_sd_lbl = NULL;
	s_orient_btn = s_orient_lbl = NULL;
	return had;
}

/* Fires for BOTH roots, and also when lv_obj_clean() on the screen takes
 * this subtree down -- which is a path no qpanel entry point sees. It logs
 * because that path is otherwise completely silent, and "the sheet vanished
 * and nothing was logged" is the one reading that takes an hour to chase:
 * the two causes (the host rebuilt its tree, or the sheet was never open to
 * begin with) look identical from the console otherwise. `had` is what keeps
 * it to one line per teardown -- the explicit path drops the pointers first,
 * so the deletes it then triggers log nothing. */
static void on_delete(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (forget()) {
		LOG_INF("qpanel: close (screen rebuilt under it)");
	}
}

static void ui_teardown(void)
{
	lv_obj_t *a = s_scrim;
	lv_obj_t *b = s_sheet;

	/* Pointers dropped BEFORE the deletes: each one fires on_delete. */
	forget();
	if (a) {
		lv_obj_delete(a);
	}
	if (b) {
		lv_obj_delete(b);
	}
	/* Deleting the pair invalidates the 240x238 they covered, so LVGL
	 * repaints that band from the tree below on the next render -- which
	 * is the whole "restore" story for the launcher. The app case is the
	 * one where there is no tree below: see glass_put_back(). */
}

/* ---------- the glass, when an app owns it ---------- */

/* Copy what is on the glass now, so it can be put back when the sheet
 * leaves. False means DO NOT OPEN -- not "open and hope". */
static bool snapshot_take(void)
{
	const uint8_t *gb = NULL;
	uint32_t w = 0, h = 0, pitch = 0;
	const uint32_t lw = aster_ui_lvgl_width();
	const uint32_t lh = aster_ui_lvgl_height();

	/* The record is allowed to ALIAS a front buffer the display worker is
	 * still streaming (glass_note_full's alias_ok), so reading it before
	 * the wire is idle is a read of memory the DMA is walking. */
	aster_plat_display_flush_drain();
	if (!aster_plat_display_glass_frame(&gb, &w, &h, &pitch)) {
		/* Reachable, not theoretical: a build without
		 * CONFIG_ASTER_SCREENSHOT keeps no shadow, so the record survives
		 * only the *whole-panel* writes (a flip or a canvas present) and
		 * an app whose last present was a partial rect leaves nothing to
		 * save. Named here because "the panel does not open in this app"
		 * would otherwise read as a gesture bug -- it is not, the pull
		 * was recognised and this is the refusal. */
		LOG_WRN("qpanel: no whole-panel frame on the glass to save -- "
			"not opening (last present was partial? builds without "
			"CONFIG_ASTER_SCREENSHOT keep no shadow to fall back on)");
		return false;
	}
	/* The glass holds a PANEL-order frame of PANEL size; the copy goes back
	 * through write_at_ex, which takes LOGICAL coordinates. Those two are
	 * the same frame only at rotation 0 -- and write_at_ex(be=true) only
	 * reaches its direct path there either. Refuse rather than write a
	 * rotated frame back as if it were logical. */
	if (w != lw || h != lh || pitch != w) {
		LOG_WRN("qpanel: glass is %ux%u pitch %u, logical is %ux%u -- not opening",
			(unsigned)w, (unsigned)h, (unsigned)pitch, (unsigned)lw, (unsigned)lh);
		return false;
	}
	if (!s_snap) {
		s_snap = aster_psram_alloc((size_t)lw * lh * 2u);
		if (!s_snap) {
			LOG_ERR("qpanel: no PSRAM for the %u B glass snapshot",
				(unsigned)(lw * lh * 2u));
			return false;
		}
	}
	memcpy(s_snap, gb, (size_t)lw * lh * 2u);
	return true;
}

/* Put it back. The caller has already made sure nothing else will paint over
 * it -- see aster_qpanel_close(). */
static void glass_put_back(void)
{
	const uint32_t lw = aster_ui_lvgl_width();
	const uint32_t lh = aster_ui_lvgl_height();

	/* pitch is in PIXELS per row (write_at_ex multiplies by 2 itself);
	 * passing bytes/row makes the staging gather step twice as far per row
	 * as the buffer holds and read off its end. `true` is the byte order:
	 * the glass record is panel order, which is what the record wants. */
	aster_plat_display_write_at_ex(s_snap, 0, 0, lw, lh, lw, true);
	/* write_at_ex hands a whole-panel rect to the async worker and returns
	 * before the wire is finished. The guest resumes the moment this
	 * function returns, so without the drain its next present would race
	 * the restore out of the same slot. */
	aster_plat_display_flush_drain();
}

/* ---------- rows ---------- */
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
static void vol_show(int v)
{
	char b[8];

	snprintf(b, sizeof(b), "%d", v);
	set_label(s_vol_val, b);
}

static void on_vol(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *sl = lv_event_get_target(e);
	int v = (int)lv_slider_get_value(sl);

	/* Applied live (a codec register write), persisted on release only:
	 * dragging a 0..100 slider fires VALUE_CHANGED dozens of times, and
	 * every one of those would be an SD file write on the UI thread. */
	(void)aster_plat_audio_set_volume(v);
	vol_show(v);
	if (code == LV_EVENT_RELEASED) {
		uint8_t raw = (uint8_t)v;

		(void)aster_kv_set(QPANEL_KV_ID, QPANEL_KV_VOL, &raw, 1);
		LOG_INF("qpanel: volume %d saved", v);
	}
}

static void vol_refresh(void)
{
	int v;

	if (!s_vol_slider) {
		return;
	}
	/* Never fight the finger: while the knob is held, this slider IS the
	 * value, whatever the codec has been told. */
	if (lv_obj_has_state(s_vol_slider, LV_STATE_PRESSED)) {
		return;
	}
	v = aster_plat_audio_get_volume();
	if (v < 0) {
		v = 0;
	} else if (v > 100) {
		v = 100;
	}
	if (lv_slider_get_value(s_vol_slider) != v) {
		lv_slider_set_value(s_vol_slider, v, LV_ANIM_OFF);
		vol_show(v);
	}
}

void aster_qpanel_restore_volume(void)
{
	uint8_t raw = 0;
	size_t n = 0;

	/* kv lives on the SD card, so there is nothing to restore from until
	 * it is up. The launcher's show path is the only caller and the only
	 * place that runs on the UI thread knowing that. */
	if (!aster_storage_mounted()) {
		return;
	}
	if (aster_kv_get(QPANEL_KV_ID, QPANEL_KV_VOL, &raw, 1, &n) == 0 && n == 1 &&
	    raw <= 100) {
		(void)aster_plat_audio_set_volume((int)raw);
		LOG_INF("qpanel: volume %u restored", (unsigned)raw);
	}
}
#else
void aster_qpanel_restore_volume(void)
{
}
#endif /* CONFIG_ASTER_AUDIO */

#if IS_ENABLED(CONFIG_ASTER_WIFI)
/* Own work queue, not the system one: k_work_submit() on the sysworkq
 * would put a call that blocks for up to 50 s (connect + DHCP) in front of
 * the cpfont metric streams, which run there. Starting it lazily keeps the
 * 2 KB of internal-DRAM stack out of images that never open this panel. */
static K_THREAD_STACK_DEFINE(s_wifi_stack, 2048);
static struct k_work_q s_wifi_q;
static bool s_wifi_q_up;

static void wifi_work_fn(struct k_work *w)
{
	int err = aster_plat_wifi_reconnect();

	atomic_set(&s_wifi_busy, 0);
	LOG_INF("qpanel: wifi reconnect -> %d", err);
}
static K_WORK_DEFINE(s_wifi_work, wifi_work_fn);

static void wifi_connect_start(void)
{
	if (!s_wifi_q_up) {
		k_work_queue_start(&s_wifi_q, s_wifi_stack,
				   K_THREAD_STACK_SIZEOF(s_wifi_stack),
				   K_PRIO_PREEMPT(12), NULL);
		s_wifi_q_up = true;
	}
	if (k_work_submit_to_queue(&s_wifi_q, &s_wifi_work) < 0) {
		return; /* already queued or running: one join at a time */
	}
	atomic_set(&s_wifi_busy, 1);
	LOG_INF("qpanel: wifi reconnect dispatched");
}

static void wifi_refresh(void);

static void on_wifi_btn(lv_event_t *e)
{
	aster_wifi_status_t ws;

	ARG_UNUSED(e);
	if (atomic_get(&s_wifi_busy)) {
		return; /* the button is inert until the join resolves */
	}
	if (aster_plat_wifi_status(&ws) == 0 && ws.state == WIFI_STATE_COMPLETED) {
		/* Cheap (a net_mgmt request, no wait), unlike connect -- this
		 * is the one Wi-Fi action that may run on the UI thread. */
		LOG_INF("qpanel: wifi disconnect -> %d",
			aster_plat_wifi_disconnect());
	} else {
		wifi_connect_start();
	}
	wifi_refresh();
}

static void wifi_refresh(void)
{
	aster_wifi_status_t ws;
	char last[ASTER_WIFI_SSID_MAX + 1];
	int err;
	bool linked, remembered, busy;

	if (!s_wifi_lbl) {
		return;
	}
	err = aster_plat_wifi_status(&ws);
	linked = !err && ws.state == WIFI_STATE_COMPLETED;
	/* -EBUSY while a join is in flight, and -ENOENT when the device has
	 * never been told to join anything. Same reading either way here. */
	remembered = aster_plat_wifi_last_ssid(last, sizeof(last)) == 0;
	busy = atomic_get(&s_wifi_busy) != 0;

	set_label(s_wifi_lbl, aster_ui_wifi_word(err ? -1 : ws.state));
	dot_color(s_dot_wifi, linked ? s_pal.accent : s_pal.dim);

	if (s_wifi_sub) {
		if (linked && ws.ssid[0]) {
			char line[64];

			if (ws.ip[0]) {
				snprintf(line, sizeof(line), "%s · %s",
					 ws.ssid, ws.ip);
			} else {
				snprintf(line, sizeof(line), "%s", ws.ssid);
			}
			set_label(s_wifi_sub, line);
		} else if (remembered) {
			char line[sizeof(last) + 8];

			snprintf(line, sizeof(line), "上次 %s", last);
			set_label(s_wifi_sub, line);
		} else {
			set_label(s_wifi_sub, "无网络记录");
		}
	}

	/* One control, three readings. There is no credential store and no
	 * keyboard, so the only join this panel can offer is the network the
	 * device was last told to join (from `hw wifi connect`); with none,
	 * the button is not shown at all rather than shown dead. */
	if (!s_wifi_btn) {
		return;
	}
	if (busy) {
		lv_obj_clear_flag(s_wifi_btn, LV_OBJ_FLAG_HIDDEN);
		set_label(s_wifi_btn_lbl, "连接中");
		lv_obj_set_style_text_color(s_wifi_btn_lbl, hexc(s_pal.dim), 0);
	} else if (linked || remembered) {
		lv_obj_clear_flag(s_wifi_btn, LV_OBJ_FLAG_HIDDEN);
		set_label(s_wifi_btn_lbl, linked ? "断开" : "重连");
		lv_obj_set_style_text_color(s_wifi_btn_lbl, hexc(s_pal.fg), 0);
	} else {
		lv_obj_add_flag(s_wifi_btn, LV_OBJ_FLAG_HIDDEN);
	}
}
#endif /* CONFIG_ASTER_WIFI */

static void sd_refresh(void)
{
	bool mounted = aster_storage_mounted();

	if (!s_sd_lbl) {
		return;
	}
	set_label(s_sd_lbl, mounted ? "SD 已挂载" : "SD 未挂载");
	dot_color(s_dot_sd, mounted ? s_pal.accent : s_pal.dim);
}

static void theme_refresh(void)
{
	if (s_theme_name && s_ops && s_ops->index) {
		set_label(s_theme_name, theme_name(s_ops->index()));
	}
}

static void on_theme_btn(lv_event_t *e)
{
	int n, i;

	ARG_UNUSED(e);
	if (!s_ops || !s_ops->count || !s_ops->index || !s_ops->apply) {
		return;
	}
	n = s_ops->count();
	i = s_ops->index();
	if (n <= 1 || i < 0 || i >= n) {
		return;
	}
	i = (i + 1) % n;
	LOG_INF("qpanel: theme -> %d of %d", i, n);
	s_ops->apply(i);
	/* Nothing below may touch a widget: apply() asks the host to rebuild
	 * its tree for the new palette, and that rebuild takes this sheet
	 * down (aster_qpanel_force_close in launcher.c). The sheet has no
	 * business surviving its host's palette changing under it -- the
	 * whole screen restyles, so it closes and shows you that. */
}

/* ---------- orientation ---------- */
/* The one row that is not a readout. Everything else on this sheet reports
 * something the device already knows; this one moves the panel, which is why
 * it is a control in the title row rather than a row of its own -- and why
 * the sheet has to be taken down by its own handler (see on_orient_btn). */

/* Whether to offer it at all. Two refusals, both of them the real answer:
 *
 *  - over an app. The app declared this orientation in its manifest and the
 *    runtime turned the panel before ctx_startup() (S6). Turning it under a
 *    running app swaps the coordinate space its surface was acquired in --
 *    the app is not re-laid-out, it is re-interpreted, and the present path
 *    would be handing a 320x240 buffer to a 240x320 window.
 *  - a square panel, where there is no second answer to give. The platform
 *    refuses the same thing with the same reason (display.c logs it).
 */
static bool orient_ok(void)
{
	int dw = (int)aster_ui_lvgl_width();
	int dh = (int)aster_ui_lvgl_height();

	return s_over_launcher && dw != dh &&
	       aster_plat_display_orientation_get() >= 0;
}

/* Names the orientation you would GET, not the one you have -- the sheet's
 * buttons are all actions (关闭, 切换, 重连) and this is a button. The
 * current one is not hidden by that: the screen is right there, and its
 * shape is the thing being named. */
static void orient_refresh(void)
{
	if (!s_orient_lbl) {
		return;
	}
	set_label(s_orient_lbl, aster_plat_display_orientation_get() ==
			       ASTER_ORIENT_LANDSCAPE ? "竖屏" : "横屏");
}

static void on_orient_btn(lv_event_t *e)
{
	int cur, want, err;

	ARG_UNUSED(e);
	cur = aster_plat_display_orientation_get();
	if (cur < 0) {
		return;
	}
	want = (cur == ASTER_ORIENT_LANDSCAPE) ? ASTER_ORIENT_PORTRAIT
					     : ASTER_ORIENT_LANDSCAPE;
	err = aster_plat_display_orientation_set(want);
	if (err) {
		/* Refused, so the panel is where it was and this sheet is still
		 * true: relabel rather than tear down something that did not
		 * change. ENOTSUP is the live one (the port has no second window
		 * and no software rotation to fall back on). */
		LOG_ERR("qpanel: orientation refused (%d)", err);
		orient_refresh();
		return;
	}
	LOG_INF("qpanel: orientation -> %s", want == ASTER_ORIENT_LANDSCAPE
					    ? "landscape" : "portrait");
	/* Close, here and now, rather than letting the rebuild below finish the
	 * job a second later. The panel has already turned: every widget this
	 * sheet owns was placed against the window that just stopped being the
	 * window, and unlike the drawer tiles there is nothing waiting to
	 * reposition them. Same deletes-from-a-callback path as 关闭, which is
	 * what on_close has always done. */
	aster_qpanel_force_close();
	/* What puts the page back: the host notices the window moved (the check
	 * in aster_launcher_pump_idle) and rebuilds, which is the only thing that
	 * re-lays-out the tiles, both home blocks and the page dots. The sheet
	 * does not ask for that -- it does not know the launcher exists -- and
	 * it does not need to: a launcher showing a stale layout is that
	 * rebuild's business either way, and this path is not the only way the
	 * window moves (`hw display orient` typed at the shell lands here too). */
}

static void on_close(lv_event_t *e)
{
	ARG_UNUSED(e);
	aster_qpanel_dismiss();
}

static void on_scrim(lv_event_t *e)
{
	ARG_UNUSED(e);
	aster_qpanel_dismiss();
}

/* ---------- build ---------- */
static void build(void)
{
	lv_obj_t *scr = aster_ui_lvgl_screen();
	lv_obj_t *row, *l;
	int dw = (int)aster_ui_lvgl_width();
	int dh = (int)aster_ui_lvgl_height();
	bool has_vol = IS_ENABLED(CONFIG_ASTER_AUDIO);
	bool has_wifi = IS_ENABLED(CONFIG_ASTER_WIFI);
	bool has_theme = s_ops && s_ops->count && s_ops->index &&
			 s_ops->name && s_ops->apply && s_ops->count() > 1;
	int h = QP_TITLE_H + QP_ROW_H /* SD */ + QP_RULE;

	if (has_vol) {
		h += QP_ROW_H;
	}
	if (has_wifi) {
		h += QP_WIFI_H;
	}
	if (has_theme) {
		h += QP_ROW_H;
	}

	load_palette();
	aster_ui_font_warm(ASTER_FONT_UI_PX,
			 "快捷设置音量关闭主题切换断开重连连接中无网络记录上次横竖屏");
	if (has_theme) {
		/* The names come from the host, so they cannot be in the literal
		 * above -- warm whatever it is about to draw. */
		for (int i = 0; i < s_ops->count(); i++) {
			aster_ui_font_warm(ASTER_FONT_UI_PX, theme_name(i));
		}
	}

	/* Scrim, then sheet: children are drawn in creation order and
	 * searched in reverse, so the second of the two is both on top and
	 * the one the indev finds first. */
	s_scrim = lv_obj_create(scr);
	lv_obj_set_size(s_scrim, dw, dh);
	lv_obj_set_pos(s_scrim, 0, 0);
	lv_obj_set_style_bg_color(s_scrim, hexc(0x000000), 0);
	lv_obj_set_style_bg_opa(s_scrim, LV_OPA_60, 0);
	lv_obj_set_style_border_width(s_scrim, 0, 0);
	lv_obj_set_style_radius(s_scrim, 0, 0);
	lv_obj_set_style_pad_all(s_scrim, 0, 0);
	no_scroll(s_scrim);
	lv_obj_add_event_cb(s_scrim, on_scrim, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(s_scrim, on_delete, LV_EVENT_DELETE, NULL);

	s_sheet = lv_obj_create(scr);
	lv_obj_set_size(s_sheet, dw, h);
	lv_obj_set_pos(s_sheet, 0, 0);
	lv_obj_set_style_bg_color(s_sheet, hexc(s_pal.bg), 0);
	lv_obj_set_style_bg_opa(s_sheet, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(s_sheet, 0, 0);
	lv_obj_set_style_pad_all(s_sheet, 0, 0);
	lv_obj_set_style_pad_row(s_sheet, 0, 0);
	/* The terminator: 2 px of accent along the bottom edge. It is a
	 * border rather than an extra child so it cannot be scrolled or hit,
	 * and it is why the sheet is 2 px taller than its rows. */
	lv_obj_set_style_border_width(s_sheet, QP_RULE, 0);
	lv_obj_set_style_border_side(s_sheet, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(s_sheet, hexc(s_pal.accent), 0);
	lv_obj_set_flex_flow(s_sheet, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(s_sheet, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	no_scroll(s_sheet);
	lv_obj_add_event_cb(s_sheet, on_delete, LV_EVENT_DELETE, NULL);

	row = mk_row(s_sheet, QP_TITLE_H, true);
	l = mk_label(row, "快捷设置", s_pal.fg, 0);
	lv_obj_set_pos(l, QP_PAD_X, (QP_TITLE_H - ASTER_FONT_UI_LINE_H) / 2);
	/* Orientation first, then 关闭: the chip goes where a display setting
	 * belongs, and 关闭 keeps the corner it has always had. Taken down
	 * rather than repositioned when it is not offered -- see orient_ok(). */
	if (orient_ok()) {
		s_orient_btn = mk_button(row,
					 dw - QP_PAD_X - 56 - QP_BTN_GAP -
						 QP_ORIENT_W,
					 (QP_TITLE_H - QP_BTN_H) / 2, QP_ORIENT_W,
					 QP_BTN_H, "", on_orient_btn,
					 &s_orient_lbl);
	} else {
		s_orient_btn = s_orient_lbl = NULL;
	}
	mk_button(row, dw - QP_PAD_X - 56, (QP_TITLE_H - QP_BTN_H) / 2, 56,
		  QP_BTN_H, "关闭", on_close, NULL);

#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	if (has_vol) {
		row = mk_row(s_sheet, QP_ROW_H, true);
		l = mk_label(row, "音量", s_pal.fg, 0);
		lv_obj_set_pos(l, QP_PAD_X, QP_ROW_Y);

		s_vol_val = mk_label(row, "0", s_pal.dim, 2);
		lv_obj_set_size(s_vol_val, QP_VOL_VAL_W, ASTER_FONT_UI_LINE_H);
		lv_obj_set_pos(s_vol_val, dw - QP_PAD_X - QP_VOL_VAL_W, QP_ROW_Y);

		s_vol_slider = lv_slider_create(row);
		lv_slider_set_range(s_vol_slider, 0, 100);
		lv_obj_set_size(s_vol_slider, dw - QP_VOL_SL_X - QP_VOL_GAP -
				QP_VOL_VAL_W - QP_PAD_X, 16);
		lv_obj_set_pos(s_vol_slider, QP_VOL_SL_X, (QP_ROW_H - 16) / 2);
		lv_obj_set_style_bg_color(s_vol_slider, hexc(s_pal.surface),
					  LV_PART_MAIN);
		lv_obj_set_style_bg_color(s_vol_slider, hexc(s_pal.accent),
					  LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(s_vol_slider, hexc(s_pal.fg),
					  LV_PART_KNOB);
		lv_obj_set_style_bg_opa(s_vol_slider, LV_OPA_COVER,
					LV_PART_KNOB);
		lv_obj_add_event_cb(s_vol_slider, on_vol,
				    LV_EVENT_VALUE_CHANGED, NULL);
		lv_obj_add_event_cb(s_vol_slider, on_vol, LV_EVENT_RELEASED,
				    NULL);
	}
#endif /* CONFIG_ASTER_AUDIO */

#if IS_ENABLED(CONFIG_ASTER_WIFI)
	if (has_wifi) {
		row = mk_row(s_sheet, QP_WIFI_H, true);
		/* The dot and the state word own the first line whole; the
		 * second line is the detail, and the button sits on the right
		 * of the first -- so a long SSID and address elides against
		 * the button's column instead of running under it. */
		s_dot_wifi = mk_dot(row, QP_DOT_X, QP_WIFI_Y1 + QP_DOT_DY);
		s_wifi_lbl = mk_label(row, "WiFi", s_pal.fg, 0);
		lv_obj_set_pos(s_wifi_lbl, QP_TXT_X, QP_WIFI_Y1);
		s_wifi_sub = mk_label(row, "", s_pal.dim, 0);
		lv_obj_set_pos(s_wifi_sub, QP_TXT_X, QP_WIFI_Y2);
		lv_obj_set_width(s_wifi_sub, dw - QP_TXT_X - QP_PAD_X);
		lv_label_set_long_mode(s_wifi_sub, LV_LABEL_LONG_MODE_DOTS);

		s_wifi_btn = mk_button(row, dw - QP_PAD_X - QP_BTN_W, 7,
				       QP_BTN_W, QP_BTN_H, "重连", on_wifi_btn,
				       &s_wifi_btn_lbl);
	}
#endif /* CONFIG_ASTER_WIFI */

	if (has_theme) {
		row = mk_row(s_sheet, QP_ROW_H, true);
		l = mk_label(row, "主题", s_pal.fg, 0);
		lv_obj_set_pos(l, QP_TXT_X, QP_ROW_Y);
		s_theme_name = mk_label(row, "", s_pal.dim, 0);
		lv_obj_set_pos(s_theme_name, QP_THEME_X, QP_ROW_Y);
		lv_obj_set_width(s_theme_name,
				 dw - QP_PAD_X - QP_BTN_W - 8 - QP_THEME_X);
		lv_label_set_long_mode(s_theme_name, LV_LABEL_LONG_MODE_DOTS);
		s_theme_btn = mk_button(row, dw - QP_PAD_X - QP_BTN_W,
					(QP_ROW_H - QP_BTN_H) / 2, QP_BTN_W,
					QP_BTN_H, "切换", on_theme_btn, NULL);
	} else {
		s_theme_name = s_theme_btn = NULL;
	}

	row = mk_row(s_sheet, QP_ROW_H, false);
	s_dot_sd = mk_dot(row, QP_DOT_X, QP_ROW_Y + QP_DOT_DY);
	s_sd_lbl = mk_label(row, "SD", s_pal.fg, 0);
	lv_obj_set_pos(s_sd_lbl, QP_TXT_X, QP_ROW_Y);

#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	if (has_vol) {
		lv_slider_set_value(s_vol_slider, aster_plat_audio_get_volume(),
				    LV_ANIM_OFF);
		vol_show(aster_plat_audio_get_volume());
	}
#endif
#if IS_ENABLED(CONFIG_ASTER_WIFI)
	if (has_wifi) {
		wifi_refresh();
	}
#endif
	if (has_theme) {
		theme_refresh();
	}
	orient_refresh(); /* no-op when the chip was not built */
	sd_refresh();
}

/* ---------- public API ---------- */
void aster_qpanel_bind_theme(const aster_qpanel_theme_ops_t *ops)
{
	s_ops = ops;
}

uint32_t aster_qpanel_allow(bool launcher)
{
	/* Pull-down is live in every context and in every state: with the
	 * sheet up it is the way out, so turning it off would strand the
	 * user. Everything else belongs to the context under the finger --
	 * page swipes at home, pull-up-exit inside an app -- and neither is
	 * live while the sheet owns the glass. */
	uint32_t m = ASTER_GEST_ALLOW_PANEL;

	if (s_sheet) {
		return m;
	}
	return m | (launcher ? ASTER_GEST_ALLOW_PAGE : ASTER_GEST_ALLOW_EXIT);
}

bool aster_qpanel_on_gesture(aster_gest_t g, aster_qpanel_host_t host)
{
	if (g != ASTER_GEST_PANEL_DOWN) {
		return false;
	}
	if (s_sheet) {
		aster_qpanel_dismiss();
	} else {
		aster_qpanel_open(host);
	}
	return true;
}

bool aster_qpanel_is_open(void)
{
	return s_sheet != NULL;
}

bool aster_qpanel_app_holds_glass(void)
{
	return s_frozen;
}

void aster_qpanel_open(aster_qpanel_host_t host)
{
	if (s_sheet || !aster_ui_lvgl_screen()) {
		return;
	}
	if (host == ASTER_QPANEL_OVER_APP && !snapshot_take()) {
		return; /* refused: see snapshot_take */
	}
	s_over_launcher = (host == ASTER_QPANEL_OVER_LAUNCHER);
	build();
	/* Armed only now that there is a sheet to shield, and only over an
	 * app: the launcher's own tree is what repaints its band, and freezing
	 * would stop nothing there anyway -- a guest present is not what puts
	 * pixels back under the sheet. */
	s_frozen = (host == ASTER_QPANEL_OVER_APP && s_sheet != NULL);
	LOG_INF("qpanel: open%s", s_frozen ? " (over app)" : "");
}

void aster_qpanel_close(bool restore)
{
	/* Two different repairs, and which one is right is decided by what is
	 * under the sheet, not by who called.
	 *
	 * Over the launcher: deleting the pair invalidates the band they
	 * covered and LVGL repaints it from a tree that is still there.
	 * Nothing to do.
	 *
	 * Over an app: there is no tree. LVGL's next refresh would paint the
	 * (emptied) screen over the guest's pixels, so the invalidated band has
	 * to be rendered FIRST and the snapshot put back SECOND. Doing it the
	 * other way round is the bug this ordering exists to avoid -- and it
	 * would look like an intermittently flickering band, not like a
	 * layering mistake. */
	if (!s_sheet && !s_scrim) {
		return;
	}
	ui_teardown();
	if (s_frozen) {
		if (restore) {
			lv_refr_now(NULL); /* settle the invalidation above */
			aster_plat_display_flush_drain();
			glass_put_back();
		}
		s_frozen = false;
	}
	LOG_INF("qpanel: close");
}

void aster_qpanel_on_app_stop(void)
{
	/* force_close is the whole point: putting the saved frame back would
	 * paint the app's last picture over the launcher that is about to
	 * replace it. */
	aster_qpanel_force_close();
	/* Belt as well as braces. A close that took the early return above
	 * would leave the flag armed, and the next app would start with its
	 * presents already swallowed -- a black screen with no obvious cause. */
	s_frozen = false;
}

void aster_qpanel_dismiss(void)
{
	aster_qpanel_close(true);
}

void aster_qpanel_force_close(void)
{
	aster_qpanel_close(false);
}

void aster_qpanel_tick(void)
{
	/* Self-throttled to 1 Hz rather than left to the caller's gate. There
	 * are two callers now and they run at completely different rates: the
	 * launcher's home refresh is already once a second, while the app loop
	 * calls this every pass. Both of the rows below are net_mgmt round
	 * trips, not variable reads, so "how often" is the sheet's business and
	 * not something two call sites should each have to remember. */
	static uint32_t last_ms;
	uint32_t now = (uint32_t)k_uptime_get();

	if (!s_sheet || (now - last_ms) < 1000u) {
		return;
	}
	last_ms = now;
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	vol_refresh();
#endif
#if IS_ENABLED(CONFIG_ASTER_WIFI)
	wifi_refresh();
#endif
	sd_refresh();
}
