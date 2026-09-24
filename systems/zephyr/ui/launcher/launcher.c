#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * Built-in native launcher (LVGL). Replaces the retired
 * "aster.launcher.desktop" guest app: same home + drawer, five themes and
 * kv-persisted state, drawn with LVGL directly on the firmware worker
 * thread. Feature-equivalent port; a real status bar / long-press
 * uninstall / animations are follow-up work.
 */
#include "launcher.h"
#include "gesture.h"
#include "qpanel.h"
#include "ui_geom.h"
#include "ui_lvgl.h"
#include "ui_font.h"
#include "font_sd.h"
#include "ui_splash.h"
#include "ui_wifi.h"
#include "aster_mem.h"
#include "aster_storage.h"
#include "aster_app_mgr.h"
#include "aster_runtime.h"
#include "aster_kv.h"
#include "aster_sysinfo.h"
#include "aster_platform.h"

#if IS_ENABLED(CONFIG_ASTER_WIFI)
/* The WiFi readout needs WIFI_STATE_COMPLETED; ui_wifi.h holds the switch
 * and this UI is Chinese. */
#include <zephyr/net/wifi.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <aster/event.h>
#include <aster/icon.h>
#include <aster/system.h>

LOG_MODULE_REGISTER(aster_launcher, LOG_LEVEL_INF);

#define LAUNCHER_KV_ID    "aster.launcher.desktop" /* keep legacy kv files */
#define LAUNCHER_N_THEMES 5
#define LAUNCHER_MAX_APPS ASTER_APP_MAX_RECS
#define LAUNCHER_ICON_MAX ASTER_APP_MAX_RECS

/* Drawer grid: full-bleed container at y=40 (the page dots sit at dh-20), an
 * 8 px right gutter the scrollbar lives in, 2 px top/bottom padding, 6 px
 * between rows, 0 between columns. Every number below is DERIVED from the
 * logical panel geometry rather than written down.
 *
 * That is not tidiness. The constants used to be literals with a BUILD_ASSERT
 * against a literal, which meant that when the panel's geometry changed the
 * assert still passed: 116*2+8 == 240 was true and stayed true while the grid
 * was being laid out on a 320-wide screen. It kept "working" -- as a 240-wide
 * block with 80 px of dead space -- and an oversized tile wraps to the next
 * line, which silently turns the grid back into the one-column list this page
 * replaced. Pinning the arithmetic to the devicetree instead makes that
 * compile error.
 *
 * The derivation reproduces portrait's historical 116x123 exactly (and the
 * regression test for this whole refactor is that portrait screenshots stay
 * byte-identical), which is the evidence that it generalises rather than
 * re-tunes.
 *
 * Below, the derivation is written ONCE as macros that take the window, and
 * evaluated twice: the plain names are it at the window this firmware was
 * BUILT for (they feed the asserts and nothing else, now), and
 * launcher_geom() is the same text at the window the panel is showing right
 * now. A turn that happens under the UI -- `hw display orient`, or the
 * quick-settings sheet's own 方向 button -- needs the second one, and the
 * only way to keep them from drifting is for the arithmetic to have one
 * copy. The asserts stay because a face re-bake that breaks the grid must
 * still break the BUILD; the runtime half cannot catch that one, since it
 * never sees the window the tree was not compiled for unless someone asks
 * the panel to show it. */
#define LAUNCHER_COLS_FOR(w, h) ((w) > (h) ? 3 : 2)
#define LAUNCHER_GRID_COLS LAUNCHER_COLS_FOR(ASTER_UI_W, ASTER_UI_H)

/* COLS is a design choice: what the phone-grid pitch should be. Landscape
 * gets 3 across -- and portrait 2, not because 3 looks wrong there but
 * because it cannot fit: a third of a 240 px panel is 77 px, narrower than
 * the 96 px icon well (asserted below). ROWS is not a choice at all: it is
 * the most rows that still leave the well full size, so 240x320 yields 2 and
 * 320x240 yields exactly 1. */
#define LAUNCHER_TILE_GAP 6
#define LAUNCHER_GRID_GUT 8
#define LAUNCHER_WELL_PX  96
/* Icon square to label. Was 4, which was right while the label was a 14 px
 * face whose line box was 19 tall; the single baked face has a 26 px line
 * box (advY), and 96 + 4 + 26 = 126 does not fit the 123 px tile. Flex
 * centring would then shift the column up by half the overflow and cut the
 * descenders off the app name.
 *
 * The 22 px that buys back is spent here rather than on the well: the well
 * is the icon's 96 px art and the icon generator draws exactly 96x96, so
 * shrinking it would drop every icon into launcher.c's `side > WELL` branch
 * and put it through the transform path for a 4% shrink (see
 * sdk/assets/icons/README.md). And it costs nothing visually -- a 26 px
 * line box has 6 px of empty lead above its ink, so the icon-to-text gap
 * only moves from 8 px to 7. */
#define LAUNCHER_TILE_PAD_ROW 1

/* Well + gap + one line box: the shortest tile that shows the icon at full
 * size and still fits the label's descenders. Window-independent, so it is
 * the same expression in both evaluations and needs no parameter. */
#define LAUNCHER_TILE_MIN_H \
	(LAUNCHER_WELL_PX + LAUNCHER_TILE_PAD_ROW + ASTER_FONT_UI_LINE_H)

#define LAUNCHER_GRID_H_(h)     ((h) - 64)   /* 40 down to the page dots   */
#define LAUNCHER_GRID_BOX_H_(gh) ((gh) - 2 - 2) /* inside top/bottom pads  */
#define LAUNCHER_TILE_W_(gw, cols) (((gw) - LAUNCHER_GRID_GUT) / (cols))
#define LAUNCHER_GRID_ROWS_(gh) \
	((LAUNCHER_GRID_BOX_H_(gh) + LAUNCHER_TILE_GAP) / \
	 (LAUNCHER_TILE_MIN_H + LAUNCHER_TILE_GAP))
#define LAUNCHER_TILE_H_(gh) \
	((LAUNCHER_GRID_BOX_H_(gh) - (LAUNCHER_GRID_ROWS_(gh) - 1) * \
	  LAUNCHER_TILE_GAP) / LAUNCHER_GRID_ROWS_(gh))

/* The build-time evaluation. Read by the asserts below and by nothing else:
 * every placement is s_geom now. */
#define LAUNCHER_GRID_W     ASTER_UI_W          /* container, full-bleed      */
#define LAUNCHER_GRID_H     LAUNCHER_GRID_H_(ASTER_UI_H)
#define LAUNCHER_GRID_BOX_H LAUNCHER_GRID_BOX_H_(LAUNCHER_GRID_H)
#define LAUNCHER_TILE_W \
	LAUNCHER_TILE_W_(LAUNCHER_GRID_W, LAUNCHER_GRID_COLS)
#define LAUNCHER_GRID_ROWS  LAUNCHER_GRID_ROWS_(LAUNCHER_GRID_H)
#define LAUNCHER_TILE_H     LAUNCHER_TILE_H_(LAUNCHER_GRID_H)
/* The tile is a COLUMN flex of well + pad + label. When that exceeds the
 * tile the overflow is silent -- the label just loses its descenders -- so
 * a re-bake that changes the face's line box has to break the build here
 * instead. That is the whole reason ASTER_FONT_UI_LINE_H is exported. */
BUILD_ASSERT(LAUNCHER_TILE_MIN_H <= LAUNCHER_TILE_H,
	     "drawer tile no longer fits: shrink LAUNCHER_WELL_PX (and re-run "
	     "sdk/tools/app_icons.py at that size) or the label's line box");
BUILD_ASSERT(LAUNCHER_GRID_ROWS >= 1,
	     "not even one row of tiles fits: the grid container is shorter "
	     "than a full-size icon well plus its label");
/* A tile narrower than the well squashes the icon through launcher.c's
 * transform path -- the one thing LAUNCHER_WELL_PX exists to prevent -- so
 * adding a column stops being free well before the width runs out. This is
 * what rules 3 columns out in portrait. */
BUILD_ASSERT(LAUNCHER_TILE_W >= LAUNCHER_WELL_PX,
	     "drawer tile is narrower than the icon well: use fewer columns");
/* These two are what make the derivation trustworthy: an integer division
 * that does not come out even leaves a strip of dead space (or pushes the
 * last tile onto its own line, which is the silent failure this whole block
 * exists for), and neither shows up as anything but "slightly wrong" on a
 * screen. Exact fill or no build. */
BUILD_ASSERT(LAUNCHER_TILE_W * LAUNCHER_GRID_COLS + LAUNCHER_GRID_GUT
	     == LAUNCHER_GRID_W,
	     "drawer tiles no longer span the panel width");
BUILD_ASSERT(LAUNCHER_TILE_H * LAUNCHER_GRID_ROWS
	     + (LAUNCHER_GRID_ROWS - 1) * LAUNCHER_TILE_GAP
	     == LAUNCHER_GRID_BOX_H,
	     "drawer rows no longer fill the grid container");

/* ---------- the same derivation, at the window on the glass now ----------
 *
 * Every value here has a BUILD_ASSERT above, and the asserts cannot see this
 * one: they are evaluated at ASTER_UI_W/ASTER_UI_H, and the whole point of this
 * path is a window that is NOT that. So the same four properties are checked
 * out loud, at the moment of use, on the numbers actually being handed to
 * lv_obj_set_size().
 *
 * Two of them are worse than wrong-looking when they fail. `rows` of 0 makes
 * the tile height a division by zero, so it is clamped rather than trusted;
 * and an uneven tile division leaves a strip of dead width, which is the
 * failure the compile-time pair exists for -- it reads as "slightly off",
 * never as broken.
 *
 * Frozen-free and integer-only on purpose: this runs on the UI thread, from
 * a pump pass, and takes no lock. */
typedef struct {
	int cols, grid_w, grid_h, box_h, tile_w, tile_h, rows;
} launcher_geom_t;

static launcher_geom_t launcher_geom(int w, int h)
{
	launcher_geom_t g;

	g.cols = LAUNCHER_COLS_FOR(w, h);
	g.grid_w = w;
	g.grid_h = LAUNCHER_GRID_H_(h);
	g.box_h = LAUNCHER_GRID_BOX_H_(g.grid_h);
	g.tile_w = LAUNCHER_TILE_W_(g.grid_w, g.cols);
	g.rows = LAUNCHER_GRID_ROWS_(g.grid_h);
	if (g.rows < 1) {
		g.rows = 1;
	}
	g.tile_h = LAUNCHER_TILE_H_(g.grid_h);

	if (g.tile_h < LAUNCHER_TILE_MIN_H) {
		LOG_ERR("launcher: window %dx%d leaves %d px tiles, need %d -- "
			"the labels will lose their descenders", w, h, g.tile_h,
			LAUNCHER_TILE_MIN_H);
	}
	if (g.tile_w < LAUNCHER_WELL_PX) {
		LOG_ERR("launcher: window %dx%d gives %d px tiles over %d columns, "
			"narrower than the %d px icon well", w, h, g.tile_w, g.cols,
			LAUNCHER_WELL_PX);
	}
	if (g.tile_w * g.cols + LAUNCHER_GRID_GUT != g.grid_w) {
		LOG_WRN("launcher: %d-column tiles span %d of %d px", g.cols,
			g.tile_w * g.cols + LAUNCHER_GRID_GUT, g.grid_w);
	}
	if (g.tile_h * g.rows + (g.rows - 1) * LAUNCHER_TILE_GAP != g.box_h) {
		LOG_WRN("launcher: %d rows fill %d of %d px", g.rows,
			g.tile_h * g.rows + (g.rows - 1) * LAUNCHER_TILE_GAP,
			g.box_h);
	}
	return g;
}

/* Chrome icons (sdk/tools/launcher_font_gen.py). All text renders through the
 * SD cpfont bake, which holds exactly one face, so every mk_label() below
 * names it by constant. Distinct px arguments used to stand in for a size
 * hierarchy (a 48 px hero clock over a 14 px status line); with one face that
 * hierarchy is not expressible, and keeping the old numbers around would
 * describe a layout that no longer exists. aster_ui_font() would snap anything
 * else here to the same face anyway -- naming it is just the honest form. */
#include "launcher_icons.inc"

typedef struct {
	uint32_t bg, clock, dim, tert, status, accent, btn_fg;
	uint32_t pill_fg, pill_bg, pill_border, pill_dot;
	uint32_t ico, ico_dim;
	uint32_t card_bg, card_fg, count;
} launcher_theme_t;

static const launcher_theme_t s_themes[LAUNCHER_N_THEMES] = {
	{ 0x0A0E1A, 0xE6EBF5, 0x5A6478, 0x8B95A8, 0x10B981, 0x06B6D4, 0xFFFFFF,
	  0xE6EBF5, 0x131826, 0x3A4660, 0x10B981, 0x8B95A8, 0x5A6478,
	  0x243044, 0xE6EBF5, 0x8B95A8 }, /* dark */
	{ 0x000000, 0xE0E0E0, 0x555555, 0x888888, 0x00FF88, 0x00FF88, 0x0A0A0A,
	  0xE0E0E0, 0x0A0A0A, 0x3A3A3A, 0x00FF88, 0x888888, 0x555555,
	  0x222222, 0xE0E0E0, 0x888888 }, /* amoled */
	{ 0xE8EEF4, 0x0F172A, 0x94A3B8, 0x64748B, 0x10B981, 0x2563EB, 0xFFFFFF,
	  0x0F172A, 0xFFFFFF, 0x94A3B8, 0x10B981, 0x64748B, 0x94A3B8,
	  0xFFFFFF, 0x0F172A, 0x64748B }, /* light */
	{ 0x0A1A0A, 0x66FF66, 0x226622, 0x44AA44, 0x00FF00, 0x00FF00, 0x0D1F0D,
	  0x66FF66, 0x102810, 0x2A6A2A, 0x00FF00, 0x44AA44, 0x226622,
	  0x1A4A1A, 0x66FF66, 0x44AA44 }, /* retro */
	{ 0x140C08, 0xF5E6E0, 0x7A6560, 0xB89D92, 0x10B981, 0xFF7A3D, 0xFFFFFF,
	  0xF5E6E0, 0x261813, 0x5A4038, 0x10B981, 0xB89D92, 0x7A6560,
	  0x3D281E, 0xF5E6E0, 0xB89D92 }, /* sunset */
};

/* ---------- launcher state (UI/worker thread unless noted) ---------- */
static int      s_theme;
static int      s_page;            /* 0 = home, 1 = drawer */
static int      s_visible;
static int      s_visible_want = 1; /* idle = home (boot path sets it again) */
static uint64_t s_show_hold_ms;	  /* first-paint gate deadline (uptime ms)  */
static int      s_res_ready;
static uint32_t s_last_sec = 0xFFFFFFFFu;
static char     s_soc[24];
static char     s_apps[LAUNCHER_MAX_APPS][64];
static int      s_app_n;
static volatile bool s_dirty;      /* registry changed: set from any thread */
static volatile int  s_want_page = -1; /* debug page switch (-1: none) */

/* The derivation the tree standing now was placed with, and the window it
 * came from. Kept rather than recomputed because the interesting question is
 * not "what is the geometry" but "is it still the one the tree was built
 * with" -- a window that moved under an already-positioned tree is a rebuild,
 * not a repaint. See the check in aster_launcher_pump_idle(). */
static launcher_geom_t s_geom;
static int s_geom_w, s_geom_h;

static lv_obj_t *s_home, *s_drawer, *s_list;
/* Home is an information page, so everything here is a status readout; the
 * drawer keeps only its app counter. s_lb_wifi_d is the second WiFi line
 * (SSID and address) and is hidden rather than blanked when there is no
 * link. */
static lv_obj_t *s_lb_clock, *s_lb_up, *s_lb_soc;
static lv_obj_t *s_lb_wifi, *s_lb_wifi_d, *s_lb_sd;
static lv_obj_t *s_dot_wifi, *s_dot_sd;
static lv_obj_t *s_drawer_n;
/* the glyph-free "no usable face" strip and the state it last reported */
static lv_obj_t *s_font_warn;
static int       s_font_warned = -1;

/* decoded app-icon blobs (PSRAM, freed on rebuild/hide) */
static lv_image_dsc_t s_icon_dsc[LAUNCHER_ICON_MAX];
static uint8_t       *s_icon_buf[LAUNCHER_ICON_MAX];
static int            s_icon_n;

/* A8 chrome descriptors over rodata (built once) */
enum { ICO_WIFI, ICO_BT, ICO_SD, ICO_THEME, ICO_N };
static lv_image_dsc_t s_chrome[ICO_N];

static lv_color_t hexc(uint32_t c)
{
	return lv_color_hex(c);
}

static void no_scroll(lv_obj_t *o)
{
	lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static void flat(lv_obj_t *o, uint32_t bg)
{
	lv_obj_set_style_bg_color(o, hexc(bg), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_set_style_radius(o, 0, 0);
	no_scroll(o);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, uint32_t color,
			  uint8_t px, int align)
{
	lv_obj_t *o = lv_label_create(parent);

	lv_label_set_text(o, text);
	lv_obj_set_style_text_color(o, hexc(color), 0);
	lv_obj_set_style_text_font(o, aster_ui_font(px), 0);
	aster_ui_font_warm(px, text);
	lv_obj_set_style_text_align(o,
		align == 1 ? LV_TEXT_ALIGN_CENTER :
		align == 2 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
	return o;
}

static void mk_icon(lv_obj_t *parent, int x, int y, const lv_image_dsc_t *dsc,
		    uint32_t color)
{
	lv_obj_t *o = lv_image_create(parent);

	lv_image_set_src(o, dsc);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_style_image_recolor(o, hexc(color), 0);
	lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
	lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

/* The little round state dot to the left of a home-page status row. Always
 * fully opaque -- the colour carries the state, and a dimmed dot would read as
 * a third state. dot_color() recolours it later; the initial value is the
 * "off" colour so a dot that is never refreshed looks off rather than
 * invisible. */
static lv_obj_t *status_dot(lv_obj_t *parent, int x, int y)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_set_size(o, 6, 6);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_style_bg_color(o, hexc(s_themes[s_theme].dim), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, 3, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	no_scroll(o);
	return o;
}

/* 16 B ASTI header + A8 payload in rodata -> LVGL descriptor. Same
 * validation as the on-disk path: a malformed blob leaves the descriptor
 * zeroed, so the image widget finds no decoder and draws nothing instead of
 * walking a geometry read out of the payload. */
static void chrome_desc(int idx, const uint8_t *buf, size_t len)
{
	lv_image_dsc_t *d = &s_chrome[idx];
	aster_icon_hdr_t h;

	memset(d, 0, sizeof(*d));
	/* chrome icons are the same A8 line-art family as the drawer tiles */
	if (aster_icon_parse(buf, len, &h) != ASTER_OK || h.cf != ASTER_ICON_CF_A8) {
		LOG_WRN("launcher: chrome icon %d malformed, dropped", idx);
		return;
	}
	d->header.magic = LV_IMAGE_HEADER_MAGIC;
	d->header.cf = LV_COLOR_FORMAT_A8;
	d->header.w = h.w;
	d->header.h = h.h;
	d->header.stride = d->header.w;
	d->data_size = (uint32_t)d->header.w * d->header.h;
	d->data = buf + ASTER_ICON_HDR_SIZE;
}

/* ---------- app icon decode (own PSRAM, not the shared asset cache) ---------- */
static int decode_app_icon(const char *app_id, lv_image_dsc_t *dsc)
{
	char dir[192];
	char path[224];
	struct fs_dirent st;
	uint8_t *file;
	aster_icon_hdr_t h;
	lv_color_format_t cf;
	uint32_t stride;
	int err;

	if (s_icon_n >= LAUNCHER_ICON_MAX) {
		return -1;
	}
	aster_storage_app_dir(dir, sizeof(dir), app_id);
	if (snprintf(path, sizeof(path), "%s/icon.bin", dir) >= (int)sizeof(path)) {
		return -1;
	}
	if (fs_stat(path, &st) != 0 || st.size < ASTER_ICON_HDR_SIZE) {
		return -1;
	}
	file = aster_psram_alloc((size_t)st.size);
	if (!file) {
		return -1;
	}
	{
		struct fs_file_t f;

		fs_file_t_init(&f);
		err = fs_open(&f, path, FS_O_READ);
		if (!err) {
			err = aster_fs_read_safe(&f, file, (size_t)st.size);
			fs_close(&f);
		}
		if (err < 0 || (size_t)err != (size_t)st.size) {
			aster_psram_free(file);
			return -1;
		}
	}
	/* icon.h checks magic/version/geometry/format and that the declared
	 * payload length matches and fits in the file, so nothing below sizes a
	 * descriptor from header bytes a corrupt file got to choose freely. */
	if (aster_icon_parse(file, (size_t)st.size, &h) != ASTER_OK ||
	    h.w > 128 || h.h > 128) {
		aster_psram_free(file);
		return -1;
	}
	/* Which formats the drawer can actually paint. RGB565/A8 carry their
	 * own colour and get blitted as they are; the legacy A8 masks still
	 * decode so a package built before the drawer went full colour shows a
	 * tinted glyph rather than an empty tile. I1 stays refused -- nothing
	 * down here renders a 1bpp mask, and a wrong stride is a silent smear
	 * rather than a visible failure. */
	switch (h.cf) {
	case ASTER_ICON_CF_A8:
		cf = LV_COLOR_FORMAT_A8;
		stride = h.w;
		break;
	case ASTER_ICON_CF_RGB565:
		cf = LV_COLOR_FORMAT_RGB565;
		stride = (uint32_t)h.w * 2u;
		break;
	case ASTER_ICON_CF_RGB565A8:
		/* LVGL locates the alpha plane at data + stride * h, which is
		 * exactly where ASTI packs it (lv_image_decoder.c,
		 * img_width_to_stride: RGB565A8 strides at w * 2). */
		cf = LV_COLOR_FORMAT_RGB565A8;
		stride = (uint32_t)h.w * 2u;
		break;
	default:
		aster_psram_free(file);
		return -1;
	}
	memset(dsc, 0, sizeof(*dsc));
	dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
	dsc->header.cf = cf;
	dsc->header.w = h.w;
	dsc->header.h = h.h;
	dsc->header.stride = stride;
	dsc->data_size = h.data_size;
	dsc->data = file + ASTER_ICON_HDR_SIZE;
	s_icon_buf[s_icon_n++] = file;
	return 0;
}

static void icons_release(void)
{
	for (int i = 0; i < s_icon_n; i++) {
		aster_psram_free(s_icon_buf[i]);
		s_icon_buf[i] = NULL;
	}
	s_icon_n = 0;
}

/* ---------- state / actions ---------- */
static void persist(void)
{
	char t = (char)s_theme;
	uint8_t p = (uint8_t)s_page;

	(void)aster_kv_set(LAUNCHER_KV_ID, "theme", &t, 1);
	(void)aster_kv_set(LAUNCHER_KV_ID, "page", &p, 1);
}

static void set_page_visual(int page)
{
	s_page = page ? 1 : 0;
	if (s_home && s_drawer) {
		if (s_page) {
			lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
			lv_obj_clear_flag(s_drawer, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_clear_flag(s_home, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(s_drawer, LV_OBJ_FLAG_HIDDEN);
		}
	}
}

static void go_page(int page)
{
	set_page_visual(page);
	persist();
}

static void on_click(lv_event_t *e)
{
	int act = (int)(intptr_t)lv_event_get_user_data(e);

	switch (act) {
	case 1: /* apps_btn / dot1 -> drawer */
		go_page(1);
		break;
	case 2: /* back_btn / ddot0 -> home */
		go_page(0);
		break;
	case 3: /* palette -> cycle theme (deferred rebuild: never delete
		 * the tree mid-event-dispatch; 10 ms later, invisible) */
		s_theme = (s_theme + 1) % LAUNCHER_N_THEMES;
		s_page = 0; /* parity with v2.3.1: theme switch lands home */
		persist();
		s_dirty = true;
		break;
	default: /* 100 + idx -> launch app (worker-thread callback:
		  * aster_app_start would self-deadlock -> sysworkq) */
		if (act >= 100 && (act - 100) < s_app_n) {
			persist();
			aster_runtime_launch_async(s_apps[act - 100]);
		}
		break;
	}
}

/* ---------- drawer ---------- */
/* One tile = the app's icon square, name underneath. The tile itself is
 * transparent and borderless: it is only the hit target and the carrier of
 * the pressed state, which it trickles down to the icon.
 *
 * The icon square is where the theming stops. A full-colour icon is the
 * package's own art -- gradient, glyph and rounded corners all baked in by
 * sdk/tools/app_icons.py -- and the launcher blits it untouched, so an app
 * looks the same under every theme. Only the labels and the chrome around
 * them still follow the palette. */
static void build_drawer(void)
{
	const launcher_theme_t *th = &s_themes[s_theme];
	char buf[48];
	int total = aster_app_mgr_count();

	if (!s_list) {
		return;
	}
	icons_release();
	lv_obj_clean(s_list);
	s_app_n = 0;
	for (int i = 0; i < total && s_app_n < LAUNCHER_MAX_APPS; i++) {
		const aster_app_rec_t *r = aster_app_mgr_get(i);
		lv_obj_t *tile, *well, *lbl;
		bool has_icon, colour;
		int idx;

		if (!r || !r->id[0]) {
			continue;
		}
		idx = s_app_n;
		snprintf(s_apps[idx], sizeof(s_apps[0]), "%s", r->id);

		tile = lv_obj_create(s_list);
		lv_obj_set_size(tile, s_geom.tile_w, s_geom.tile_h);
		lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(tile, 0, 0);
		lv_obj_set_style_pad_all(tile, 0, 0);
		lv_obj_set_style_pad_row(tile, LAUNCHER_TILE_PAD_ROW, 0);
		lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
		/* The tile has no background of its own to show PRESSED, so the
		 * state has to reach the well and the icon inside it. TRICKLE
		 * goes on the object that *starts* the trickle and covers the
		 * whole subtree below it -- including descendants that do not
		 * carry the flag themselves. (Upstream stopped at the first
		 * child without it; see sdk/vendor/lvgl/PATCHES.md.) */
		lv_obj_add_flag(tile, LV_OBJ_FLAG_STATE_TRICKLE);
		/* A name wider than the tile would otherwise make the tile itself
		 * scrollable and pan its own contents on a drag. */
		no_scroll(tile);
		lv_obj_add_event_cb(tile, on_click, LV_EVENT_CLICKED,
				    (void *)(intptr_t)(100 + idx));

		/* Test the return value, not "does the well have children":
		 * decode_app_icon only zeroes the descriptor on success, so a
		 * failed decode leaves the previous rebuild's blob in place.
		 * Decoded before the widgets exist because the format decides
		 * how the square under the icon gets styled. */
		has_icon = r->has_icon &&
			   decode_app_icon(r->id, &s_icon_dsc[idx]) == 0;
		colour = has_icon &&
			 s_icon_dsc[idx].header.cf != LV_COLOR_FORMAT_A8;

		/* Created unconditionally: an app whose icon failed to decode
		 * still gets its square, so one bad package cannot pull the grid
		 * off its two-column pitch. */
		well = lv_obj_create(tile);
		lv_obj_set_size(well, LAUNCHER_WELL_PX, LAUNCHER_WELL_PX);
		lv_obj_set_style_border_width(well, 0, 0);
		lv_obj_set_style_radius(well, 26, 0);
		lv_obj_set_style_pad_all(well, 0, 0);
		no_scroll(well);
		lv_obj_clear_flag(well, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_flag(well, LV_OBJ_FLAG_EVENT_BUBBLE);
		/* No TRICKLE here: the tile's flag already covers this level and
		 * the icon below it. A second one would hide a regression in that
		 * propagation instead of surfacing it. */
		if (colour) {
			/* The art fills the square corner to corner and brings
			 * its own background. Showing the theme's surface under
			 * it would halo through the rounded corners, and lighting
			 * it up on press would do nothing but hide the icon. */
			lv_obj_set_style_bg_opa(well, LV_OPA_TRANSP, 0);
		} else {
			/* Placeholder, or the backing a legacy A8 mask needs.
			 * Neutral card surface rather than the theme accent: the
			 * accent is what made every tile the same colour, which
			 * is the thing this page stopped doing. */
			lv_obj_set_style_bg_color(well, hexc(th->card_bg), 0);
			lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);
			/* Press state layer: the theme's bright ink at low
			 * opacity, so the feedback direction follows the theme's
			 * contrast instead of a hardcoded lightening. */
			lv_obj_set_style_bg_color(well, hexc(th->btn_fg),
						  LV_STATE_PRESSED);
			lv_obj_set_style_bg_opa(well, LV_OPA_20,
						LV_STATE_PRESSED);
		}

		if (has_icon) {
			const lv_image_dsc_t *d = &s_icon_dsc[idx];
			int32_t side = LV_MAX((int32_t)d->header.w,
					      (int32_t)d->header.h);
			lv_obj_t *img = lv_image_create(well);

			lv_image_set_src(img, d);
			lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
			if (colour) {
				/* The only press feedback a full-colour icon
				 * has: dim it. Nothing here reads the theme, so
				 * a tile looks the same in all five.
				 *
				 * image_opa, not opa: an image draws at
				 * lv_obj_get_style_image_opa() mixed with the
				 * layer's opa, and the object's own `opa` only
				 * reaches it as a layer, which this widget does
				 * not open (core/lv_obj_draw.c). */
				lv_obj_set_style_image_opa(img, LV_OPA_60,
							   LV_STATE_PRESSED);
			} else {
				/* A mask has no colour of its own, so it takes
				 * the surface's ink. */
				lv_obj_set_style_image_recolor(img,
						hexc(th->card_fg), 0);
				lv_obj_set_style_image_recolor_opa(img,
						LV_OPA_COVER, 0);
			}
			if (side <= LAUNCHER_WELL_PX) {
				lv_obj_center(img);	/* 1:1, no transform */
			} else {
				/* Larger than the square: shrink about the
				 * centre. Never the other way -- magnifying a
				 * small blob is a nearest-neighbour smear and
				 * puts every pixel through the transform path.
				 * The oversized widget is centred first, so
				 * pivoting about its middle lands the scaled
				 * result on the square's centre. */
				lv_image_set_pivot(img, d->header.w / 2,
						   d->header.h / 2);
				lv_image_set_scale(img,
					(uint32_t)(256 * LAUNCHER_WELL_PX / side));
				lv_image_set_antialias(img, true);
				lv_obj_center(img);
			}
		}
		lbl = mk_label(tile, r->name[0] ? r->name : r->id,
			       th->card_fg, ASTER_FONT_UI_PX, 1);
		lv_obj_set_width(lbl, LV_PCT(100));
		lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
		s_app_n++;
	}
	/* The app count lives here and nowhere else now. It used to be written
	 * twice: once here and once onto s_apps_lbl, the "所有应用" button on
	 * the home page. That button is gone with the information-page rebuild
	 * (navigation is a swipe), so this pairing had to go in the same commit
	 * -- removing only the build_home() half would leave the text being set
	 * on a label that no longer exists. */
	snprintf(buf, sizeof(buf), "%d 个", s_app_n);
	if (s_drawer_n) {
		lv_label_set_text(s_drawer_n, buf);
	}
}

/* Only touch a label when the text actually changed. lv_label_set_text()
 * reallocs and invalidates unconditionally, and the WiFi/SD lines change
 * maybe twice per boot while this runs at 1 Hz. */
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

/* ---------- status (1 Hz; the clock is uptime -- there is no RTC) ---------- */

static void refresh_home(void)
{
	const launcher_theme_t *th = &s_themes[s_theme];
	uint32_t s = (uint32_t)(k_uptime_get() / 1000u);
	char clock[16], up[40];

	if (s == s_last_sec || !s_lb_clock) {
		return;
	}
	s_last_sec = s;

	snprintf(clock, sizeof(clock), "%02u:%02u:%02u",
		 (unsigned)((s / 3600u) % 100u), (unsigned)((s / 60u) % 60u),
		 (unsigned)(s % 60u));
	set_label(s_lb_clock, clock);

	/* One fact per line. This started as a single "esp32s3 · 已运行 0时0分"
	 * and measured 229 px inside a 240 px label -- 11 px of headroom, which
	 * a two-digit hour would have eaten. Splitting it means neither line can
	 * reach the edge: the widest uptime ("99时59分") is about 149 px and the
	 * SoC name is set once at boot. */
	snprintf(up, sizeof(up), "已运行 %u时%u分",
		 (unsigned)((s / 3600u) % 100u), (unsigned)((s / 60u) % 60u));
	set_label(s_lb_up, up);

#if IS_ENABLED(CONFIG_ASTER_WIFI)
	{
		aster_wifi_status_t ws;
		int err = aster_plat_wifi_status(&ws);
		/* `linked`, not `up`: the uptime string above owns that name. */
		int linked = !err && ws.state == WIFI_STATE_COMPLETED;

		set_label(s_lb_wifi, aster_ui_wifi_word(err ? -1 : ws.state));
		dot_color(s_dot_wifi, linked ? th->pill_dot : th->dim);
		if (!s_lb_wifi_d) {
			/* nothing to do */
		} else if (linked && ws.ssid[0]) {
			char line[64];

			if (ws.ip[0]) {
				snprintf(line, sizeof(line), "%s · %s", ws.ssid,
					 ws.ip);
			} else {
				snprintf(line, sizeof(line), "%s", ws.ssid);
			}
			set_label(s_lb_wifi_d, line);
			lv_obj_clear_flag(s_lb_wifi_d, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(s_lb_wifi_d, LV_OBJ_FLAG_HIDDEN);
		}
	}
#endif

	{
		int mounted = aster_storage_mounted();

		set_label(s_lb_sd, mounted ? "SD 已挂载" : "SD 未挂载");
		dot_color(s_dot_sd, mounted ? th->pill_dot : th->dim);
	}

	/* The one face is missing. Checked here rather than in build_home()
	 * because the launcher can be shown while the metric table is still
	 * streaming (the 12 s gate expires first), so "missing now" is not
	 * "missing forever". refresh_home() runs at 1 Hz, so the strip appears
	 * the moment the stream dies and clears the moment it lands. */
	if (s_font_warn) {
		int missing = aster_font_sd_get(ASTER_FONT_UI_PX) == NULL;

		if (missing != s_font_warned) {
			s_font_warned = missing;
			if (missing) {
				lv_obj_clear_flag(s_font_warn, LV_OBJ_FLAG_HIDDEN);
				LOG_ERR("launcher: no usable %u px face -- "
					"ui_%u.cpfont missing or unreadable on the "
					"SD card; every label draws placeholder "
					"boxes and the red strip is up",
					(unsigned)ASTER_FONT_UI_PX,
					(unsigned)ASTER_FONT_UI_PX);
			} else {
				lv_obj_add_flag(s_font_warn, LV_OBJ_FLAG_HIDDEN);
				LOG_INF("launcher: %u px face is up",
					(unsigned)ASTER_FONT_UI_PX);
			}
		}
	}

}

/* ---------- tree ---------- */

/* Home is a PURE INFORMATION PAGE: no app entries and no "all apps" button --
 * navigation to the drawer is a swipe (firmware/ui/gesture.c). Everything on
 * it is real device state, because the page this replaces mixed in two things
 * that were not: the "USB 5V" pill was a STRING LITERAL (there is no
 * CONFIG_USB_DEVICE_STACK and no ADC) and the BT icon was decoration
 * (CONFIG_BT is not set). Both are gone rather than restyled, and so are the
 * two decorative lines that went with them ("你好" and "系统在线" -- neither
 * said anything the rest of the page did not).
 *
 * What is left is what the device can actually answer:
 *   - WiFi: aster_plat_wifi_status() is a net_mgmt query, cheap enough at 1 Hz.
 *     scan (15 s) and connect (35 s) are NOT called from here -- this runs on
 *     the thread that drives LVGL, which is why those belong behind qpanel's
 *     explicit controls and not on a page that redraws every second.
 *   - SD: aster_storage_mounted(), a flag. There is deliberately no free-space
 *     line: no such API exists, and reaching for fs_statvfs would be SD I/O
 *     on the UI thread.
 *   - the clock is UPTIME, not wall time -- there is no RTC.
 *
 * The clock is 20 px because there is exactly one baked face. The 48 px hero
 * clock this page used to carry would now draw the same 20 px glyphs into a
 * 48 px box, so the hero treatment is not available at any price short of a
 * second face -- which is what the single-face migration removed. */
/* The page is two blocks: status (WiFi over SD) and identity (clock over
 * uptime over chip name). Portrait stacks them down the full width; landscape
 * puts them side by side, because 240 px of height will not hold the stack --
 * the identity block would run into the page dots. Only each block's origin
 * and extent differ; the offsets WITHIN a block are shared, so the two
 * arrangements cannot drift apart. The portrait column here is the historical
 * absolute layout written as offsets, unchanged down to the pixel.
 *
 * These were #if'd on ASTER_UI_LANDSCAPE and are locals now, for the same
 * reason the drawer grid moved: the choice between the two arrangements is
 * the SHAPE OF THE WINDOW, and the window is not always the one this
 * firmware was compiled for. The `wide` test is the same comparison the
 * macro made. */
#define HOME_ST_DOT_DY  10
/* One line step (26) plus a clear gap, so the three lines read as a block. */
#define HOME_CL_STEP 32

typedef struct {
	int st_w, st_y, cl_x, cl_w, cl_y; /* the two block origins/extents */
	int st_y_wifi, st_y_wifi_d, st_y_sd, st_dot_x, st_text_x;
} home_geom_t;

static home_geom_t home_geom(uint32_t dw, uint32_t dh)
{
	bool wide = (dw > dh);
	home_geom_t g = {
		.st_w = wide ? (int)(dw / 2) : (int)dw,
		.st_y = wide ? 40 : 0,
		.cl_x = wide ? (int)(dw / 2) : 0,
		.cl_w = wide ? (int)(dw - dw / 2) : (int)dw,
		.cl_y = wide ? 72 : 150,
	};

	/* Line-box tops within the status block, and how far a status dot sits
	 * above its own label: the dot is centred on the label's line box
	 * (top + 13 - 3). The status block starts at x = 0 in both
	 * arrangements, which is why there is no st_x. */
	g.st_y_wifi = g.st_y + 13;
	g.st_y_wifi_d = g.st_y + 39;
	g.st_y_sd = g.st_y + 87;
	g.st_dot_x = 12;
	g.st_text_x = 28;
	return g;
}

static void build_home(void)
{
	const launcher_theme_t *th = &s_themes[s_theme];
	uint32_t dw = aster_ui_lvgl_width();
	uint32_t dh = aster_ui_lvgl_height();
	const home_geom_t hg = home_geom(dw, dh);
	lv_obj_t *scr = aster_ui_lvgl_screen();
	lv_obj_t *tb, *o;

	s_home = lv_obj_create(scr);
	lv_obj_set_size(s_home, (int32_t)dw, (int32_t)dh);
	lv_obj_set_pos(s_home, 0, 0);
	flat(s_home, th->bg);

	/* theme switcher, top-right -- chrome, not status */
	tb = lv_obj_create(s_home);
	lv_obj_set_size(tb, 44, 32);
	lv_obj_set_pos(tb, (int32_t)dw - 44, 2);
	lv_obj_set_style_bg_opa(tb, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(tb, 0, 0);
	lv_obj_set_style_pad_all(tb, 0, 0);
	no_scroll(tb);
	lv_obj_add_flag(tb, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(tb, on_click, LV_EVENT_CLICKED, (void *)3);
	/* 16x16 art centred in the 44x32 hit box: ((44-16)/2, (32-16)/2). The
	 * old (18, 10) was centred for the 52x36 button this replaced and is
	 * 4 px right of centre here. */
	mk_icon(tb, 14, 8, &s_chrome[ICO_THEME], th->ico);

	/* WiFi: dot + state word, with the SSID and address on the line below.
	 * Positions are line-box TOPS: a label's ink baseline sits asc (21)
	 * rows down from it, so one line box is 26 tall and the dot is centred
	 * on that box (top + 13 - 3). The second line is exactly one line step
	 * under the first, so the two stay together if the face is re-baked. */
	s_dot_wifi = status_dot(s_home, hg.st_dot_x, hg.st_y_wifi + HOME_ST_DOT_DY);
	s_lb_wifi = mk_label(s_home, "WiFi", th->pill_fg, ASTER_FONT_UI_PX, 0);
	lv_obj_set_pos(s_lb_wifi, hg.st_text_x, hg.st_y_wifi);
	s_lb_wifi_d = mk_label(s_home, "", th->dim, ASTER_FONT_UI_PX, 0);
	lv_obj_set_pos(s_lb_wifi_d, hg.st_text_x, hg.st_y_wifi_d);
	lv_obj_set_width(s_lb_wifi_d, hg.st_w - 28 - 8);
	lv_label_set_long_mode(s_lb_wifi_d, LV_LABEL_LONG_MODE_DOTS);

	/* SD: dot + state word. No second line -- see the note above. */
	s_dot_sd = status_dot(s_home, hg.st_dot_x, hg.st_y_sd + HOME_ST_DOT_DY);
	s_lb_sd = mk_label(s_home, "SD", th->pill_fg, ASTER_FONT_UI_PX, 0);
	lv_obj_set_pos(s_lb_sd, hg.st_text_x, hg.st_y_sd);

	/* clock over uptime over identity, centred as a block, with a clear gap
	 * above it. s_lb_soc is set from s_soc once and never rewritten -- the
	 * chip name cannot change while the device runs, so putting it through
	 * refresh_home()'s 1 Hz path would be pure waste. (s_soc itself can
	 * still be empty here: aster_launcher_show() only fills it when the SD
	 * card is mounted, so the first show on a cardless boot leaves the
	 * label blank until the next rebuild.) */
	s_lb_clock = mk_label(s_home, "00:00:00", th->clock, ASTER_FONT_UI_PX, 1);
	lv_obj_set_width(s_lb_clock, hg.cl_w);
	lv_obj_set_pos(s_lb_clock, hg.cl_x, hg.cl_y);
	s_lb_up = mk_label(s_home, "", th->tert, ASTER_FONT_UI_PX, 1);
	lv_obj_set_width(s_lb_up, hg.cl_w);
	lv_obj_set_pos(s_lb_up, hg.cl_x, hg.cl_y + HOME_CL_STEP);
	s_lb_soc = mk_label(s_home, s_soc, th->dim, ASTER_FONT_UI_PX, 1);
	lv_obj_set_width(s_lb_soc, hg.cl_w);
	lv_obj_set_pos(s_lb_soc, hg.cl_x, hg.cl_y + 2 * HOME_CL_STEP);

	/* The "the baked face is unusable" marker, created hidden and flipped by
	 * refresh_home(). Deliberately glyph-free: with the one baked face gone
	 * there is no way to write a message that is not itself rendered as the
	 * placeholder's filled boxes (ui_font.c), i.e. indistinguishable from
	 * the thing it is trying to report. A red strip along the bottom edge
	 * plus one LOG_ERR naming the file is what is actually available, and
	 * `aster sys lvgl` answers the same question on demand. */
	s_font_warn = lv_obj_create(s_home);
	lv_obj_set_size(s_font_warn, (int32_t)dw, 6);
	lv_obj_set_pos(s_font_warn, 0, (int32_t)dh - 6);
	lv_obj_set_style_bg_color(s_font_warn, hexc(0xEF4444u), 0);
	lv_obj_set_style_bg_opa(s_font_warn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(s_font_warn, 0, 0);
	lv_obj_set_style_radius(s_font_warn, 0, 0);
	no_scroll(s_font_warn);
	lv_obj_add_flag(s_font_warn, LV_OBJ_FLAG_HIDDEN);

	/* page dots: home active (right dot -> drawer) */
	o = lv_obj_create(s_home);
	lv_obj_set_size(o, 14, 6);
	lv_obj_set_pos(o, (int32_t)((dw - 20) / 2), (int32_t)dh - 20);
	lv_obj_set_style_bg_color(o, hexc(th->accent), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, 3, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	no_scroll(o);
	o = lv_obj_create(s_home);
	lv_obj_set_size(o, 6, 6);
	lv_obj_set_pos(o, (int32_t)((dw - 20) / 2 + 18), (int32_t)dh - 20);
	lv_obj_set_style_bg_color(o, hexc(th->dim), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, 3, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	no_scroll(o);
	lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(o, on_click, LV_EVENT_CLICKED, (void *)1);
}

static void build_drawer_page(void)
{
	const launcher_theme_t *th = &s_themes[s_theme];
	uint32_t dw = aster_ui_lvgl_width();
	uint32_t dh = aster_ui_lvgl_height();
	lv_obj_t *scr = aster_ui_lvgl_screen();
	lv_obj_t *back, *o, *l;

	s_drawer = lv_obj_create(scr);
	lv_obj_set_size(s_drawer, (int32_t)dw, (int32_t)dh);
	lv_obj_set_pos(s_drawer, 0, 0);
	flat(s_drawer, th->bg);

	back = lv_button_create(s_drawer);
	lv_obj_set_size(back, 68, 32);
	lv_obj_set_pos(back, 8, 8);
	lv_obj_set_style_bg_color(back, hexc(th->card_bg), 0);
	lv_obj_set_style_radius(back, 8, 0);
	lv_obj_set_style_shadow_width(back, 0, 0);
	lv_obj_add_event_cb(back, on_click, LV_EVENT_CLICKED, (void *)2);
	l = mk_label(back, "主页", th->card_fg, ASTER_FONT_UI_PX, 1);
	lv_obj_center(l);

	o = mk_label(s_drawer, "所有应用", th->card_fg, ASTER_FONT_UI_PX, 0);
	lv_obj_set_pos(o, 84, 10);
	s_drawer_n = mk_label(s_drawer, "0 个", th->count, ASTER_FONT_UI_PX, 2);
	lv_obj_set_size(s_drawer_n, 52, 28);
	lv_obj_set_pos(s_drawer_n, (int32_t)dw - 60, 10);

	s_list = lv_obj_create(s_drawer);
	/* Full-bleed grid, 40 down to the page dots: wallpaper shows between
	 * the tiles, like a phone home screen. It has to be full-bleed, not an
	 * inset card -- a frame plus its border eats 16 px of the width, which
	 * on a 240-wide panel leaves too little for even a single row of
	 * full-size tiles and lays out as one column. The 8 px right gutter is
	 * where the scrollbar goes; pad_column MUST stay 0, it is the gap
	 * between tiles on the main axis (flex puts pad_row between rows). */
	lv_obj_set_size(s_list, s_geom.grid_w, s_geom.grid_h);
	lv_obj_set_pos(s_list, 0, 40);
	lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(s_list, 0, 0);
	lv_obj_set_style_radius(s_list, 0, 0);
	lv_obj_set_style_pad_top(s_list, 2, 0);
	lv_obj_set_style_pad_bottom(s_list, 2, 0);
	lv_obj_set_style_pad_left(s_list, 0, 0);
	lv_obj_set_style_pad_right(s_list, LAUNCHER_GRID_GUT, 0);
	lv_obj_set_style_pad_row(s_list, LAUNCHER_TILE_GAP, 0);
	lv_obj_set_style_pad_column(s_list, 0, 0);
	lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
	/* Free scrolling, deliberately. With scroll snap the theme's snap-point
	 * correction is added outside the throw clamp (lv_indev_scroll.c,
	 * find_snap_point_*) while the elastic revert only runs for
	 * LV_SCROLL_SNAP_NONE, so a fling can come to rest out of bounds.
	 * Momentum, which is what actually makes it feel like a phone, is on
	 * by default. */
	lv_obj_set_scroll_snap_y(s_list, LV_SCROLL_SNAP_NONE);
	/* Park the scrollbar inside the gutter. The theme's bar is 5 px with
	 * 7 px of right padding (lv_theme_default.c), which anchors it flush
	 * against the right edge and puts 4 px of it on top of the last tile.
	 * Dropping the pad and narrowing it to 4 px lands it inside the
	 * gutter, whose width it now exactly matches. */
	lv_obj_set_style_width(s_list, LAUNCHER_GRID_GUT / 2, LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_right(s_list, 0, LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_top(s_list, 4, LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_bottom(s_list, 4, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_color(s_list, hexc(th->accent), LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(s_list, LV_OPA_50, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER,
				LV_PART_SCROLLBAR | LV_STATE_SCROLLED);

	/* page dots: drawer active (left dot -> home) */
	o = lv_obj_create(s_drawer);
	lv_obj_set_size(o, 6, 6);
	lv_obj_set_pos(o, (int32_t)((dw - 20) / 2), (int32_t)dh - 20);
	lv_obj_set_style_bg_color(o, hexc(th->dim), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, 3, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	no_scroll(o);
	lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(o, on_click, LV_EVENT_CLICKED, (void *)2);
	o = lv_obj_create(s_drawer);
	lv_obj_set_size(o, 14, 6);
	lv_obj_set_pos(o, (int32_t)((dw - 20) / 2 + 10), (int32_t)dh - 20);
	lv_obj_set_style_bg_color(o, hexc(th->accent), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, 3, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	no_scroll(o);
}

/* ---------- quick-settings sheet ---------- */
/* The sheet is a guest of this screen and knows none of the above: it asks
 * these five questions and nothing else. Everything it needs to know about
 * themes -- that there are five fixed ones, that switching one is a
 * rebuild rather than a repaint, that "the theme" is a file-static int --
 * stays on this side of the binding. */

static int qp_theme_count(void)
{
	return LAUNCHER_N_THEMES;
}

static int qp_theme_index(void)
{
	return s_theme;
}

static const char *qp_theme_name(int i)
{
	/* Named for the sheet's theme row. The indices are the order of
	 * s_themes above; the names are the only thing that says which
	 * palette "深色" is, since nothing on this screen shows them. */
	static const char *const names[LAUNCHER_N_THEMES] = {
		"深色", "纯黑", "浅色", "复古", "日落",
	};

	return (i >= 0 && i < LAUNCHER_N_THEMES) ? names[i] : "主题";
}

static void qp_theme_apply(int i)
{
	if (i < 0 || i >= LAUNCHER_N_THEMES) {
		return;
	}
	/* Exactly what the palette button at the top right does (on_click
	 * act 3): the theme switch lands on home, persists, and asks for a
	 * deferred rebuild. Not a repaint -- the palette is read into every
	 * widget at build time. That rebuild tears the sheet down with it,
	 * which is the behaviour the sheet's own comment describes. */
	s_theme = i;
	s_page = 0;
	persist();
	s_dirty = true;
}

static void qp_theme_palette(aster_qpanel_palette_t *out)
{
	int t = (s_theme >= 0 && s_theme < LAUNCHER_N_THEMES) ? s_theme : 0;
	const launcher_theme_t *th = &s_themes[t];

	/* The sheet is a raised surface over the page, so it takes the card
	 * tones and the page colour becomes the fill of the controls on it --
	 * which is what keeps the sheet legible in all five, including the
	 * light one, where a hardcoded dark sheet would be a black slab. */
	out->bg = th->card_bg;
	out->fg = th->card_fg;
	out->dim = th->count;
	out->surface = th->bg;
	out->accent = th->accent;
}

static const aster_qpanel_theme_ops_t s_qp_ops = {
	.count = qp_theme_count,
	.index = qp_theme_index,
	.name = qp_theme_name,
	.apply = qp_theme_apply,
	.palette = qp_theme_palette,
};

/* ---------- public entry points ---------- */
int aster_launcher_init(void)
{
	if (!s_res_ready) {
		chrome_desc(ICO_WIFI, launcher_ico_wifi, sizeof(launcher_ico_wifi));
		chrome_desc(ICO_BT, launcher_ico_bt, sizeof(launcher_ico_bt));
		chrome_desc(ICO_SD, launcher_ico_sd, sizeof(launcher_ico_sd));
		chrome_desc(ICO_THEME, launcher_ico_theme,
			    sizeof(launcher_ico_theme));
		/* Own the single registry-change slot: refresh the drawer. */
		aster_app_mgr_set_change_cb(aster_launcher_on_registry_change);
		aster_qpanel_bind_theme(&s_qp_ops);
		s_res_ready = 1;
	}
	return 0;
}

void aster_launcher_show(void)
{
	char t = 0;
	uint8_t p = 0;
	size_t n = 0;

	if (s_visible) {
		return;
	}
	aster_launcher_init();
	if (!aster_ui_lvgl_screen()) {
		return;
	}
	/* Before anything is built, not on the next pump. An app that declared
	 * an orientation has just had the window put back (runtime/S6), and
	 * this function reads aster_ui_lvgl_width()/height() to place the
	 * toolbar -- numbers it must not take from a display still pointing at
	 * the app's window, since they are read once, here. The pump would fix
	 * the display a frame later, which is too late for a tree that has
	 * already been positioned.
	 *
	 * Everything below this line is measured against the window this call
	 * has just made current: the drawer grid, both home arrangements and
	 * the page dots all come from launcher_geom()/home_geom(), not from
	 * ASTER_UI_W/ASTER_UI_H. That is what a runtime turn needs -- the panel can
	 * now be showing a window this firmware was not compiled for, and the
	 * page has to be laid out for it rather than merely repainted into it.
	 * The build-time pair survives in the BUILD_ASSERTs, which is the half
	 * a face re-bake should break, and the runtime half logs the same four
	 * properties when it is handed a window the asserts never saw. */
	aster_ui_lvgl_resync_window();
	s_geom = launcher_geom((int)aster_ui_lvgl_width(), (int)aster_ui_lvgl_height());
	s_geom_w = (int)aster_ui_lvgl_width();
	s_geom_h = (int)aster_ui_lvgl_height();
	/* The sheet is a child of the active screen, so lv_obj_clean() below
	 * would take it out anyway -- but take it out HERE, first. It was
	 * built over a tree that is about to stop existing, and closing it
	 * explicitly is what keeps "which of the two deletes it" from being a
	 * question anyone has to answer later. */
	aster_qpanel_force_close();
	/* A stroke that was in flight when the previous context went away must
	 * not be completed against this one: its start point, its band and its
	 * gesture mask all describe a screen that is gone. */
	aster_gesture_reset();
	if (!s_soc[0] && aster_storage_mounted()) {
		aster_system_info_t si = { .size = sizeof(si), .version = 1 };

		aster_sysinfo_get(&si);
		if (si.soc[0]) {
			strncpy(s_soc, si.soc, sizeof(s_soc) - 1);
			s_soc[sizeof(s_soc) - 1] = 0;
		}
	}
	if (aster_storage_mounted()) {
		if (aster_kv_get(LAUNCHER_KV_ID, "theme", &t, 1, &n) == 0 &&
		    n == 1 && t < LAUNCHER_N_THEMES) {
			s_theme = t;
		}
		n = 0;
		if (aster_kv_get(LAUNCHER_KV_ID, "page", &p, 1, &n) == 0 && n == 1) {
			s_page = p ? 1 : 0;
		}
		/* The volume is a device setting, not a launcher one, but this
		 * is the only place in the firmware that runs on the UI thread
		 * with "the SD is mounted" already known -- which is what any
		 * kv read needs. Putting it in aster_plat_audio_set_volume()
		 * instead would make the platform layer depend on kv and would
		 * rewrite the card on every call a guest app makes. */
		aster_qpanel_restore_volume();
	}

	/* Text: SD font router (the one baked cpfont face -- ASTER_FONT_UI_PX,
	 * see font_sd.h). Warm the digits/punct the clock and status line
	 * need right away, and every label created below is queued for
	 * burst prefetch. */
	lv_obj_clean(aster_ui_lvgl_screen());
	s_visible = 1;
	/* One face, so one warm call. The old pair (48 for the hero clock, 14 for
	 * the status line) both landed on the same cpfont slot and queued the
	 * same codepoints twice. */
	aster_ui_font_warm(ASTER_FONT_UI_PX, "0123456789:/-AMPM ");
	build_home();
	build_drawer_page();
	build_drawer();
	go_page(s_page);
	refresh_home();
	s_dirty = false;
	LOG_INF("launcher: home theme=%d page=%s apps=%d",
		s_theme, s_page ? "drawer" : "home", s_app_n);
}

void aster_launcher_hide(void)
{
	if (!s_visible) {
		return;
	}
	/* Same two as the show path, and just as load-bearing: `aster app start`
	 * from the shell can take the screen away while the sheet is up, and a
	 * sheet left hanging over the app would be operating on a tree that no
	 * longer exists (its widget pointers are into that tree). */
	aster_qpanel_force_close();
	aster_gesture_reset();
	persist();
	if (aster_ui_lvgl_screen()) {
		lv_obj_clean(aster_ui_lvgl_screen());
	}
	s_home = s_drawer = s_list = NULL;
	s_lb_clock = s_lb_up = s_lb_soc = NULL;
	s_lb_wifi = s_lb_wifi_d = s_lb_sd = NULL;
	s_dot_wifi = s_dot_sd = NULL;
	s_drawer_n = s_font_warn = NULL;
	s_font_warned = -1;
	icons_release();
	s_visible = 0;
	s_last_sec = 0xFFFFFFFFu;
}

void aster_launcher_request_show(void)
{
	s_visible_want = 1;
}

void aster_launcher_pump_idle(void)
{
    if (!aster_plat_display_count()) return;
	aster_touch_evt_t tev[ASTER_TOUCH_MAX];
	int n = aster_plat_input_poll(tev, ASTER_TOUCH_MAX);

	for (int i = 0; i < n; i++) {
		bool pressed = tev[i].action != ASTER_TOUCH_UP;
		/* Asked per sample, not once per batch: a pull-down earlier in
		 * this same batch can have just opened or closed the sheet, and
		 * that changes what the next sample is allowed to mean. */
		/* The logical height, not ASTER_UI_H. It was the macro, and the two
		 * were the same number -- which stopped being true the moment the
		 * panel could be turned without a rebuild
		 * (runtime/aster_runtime.c passes aster_ui_lvgl_height() for exactly
		 * this reason). The pull-up band is measured from the bottom edge,
		 * so a stale height puts it 80 px inside the glass on a 240x320
		 * window. */
		aster_gest_t g = aster_gesture_feed(tev[i].x, tev[i].y, pressed,
						aster_qpanel_allow(true),
						(int32_t)aster_ui_lvgl_height());

		/* Bypass the app-attach gate: at home there is no app to route
		 * to. Fed unconditionally, gestures included -- LVGL's indev
		 * only ever sees press/release pixels, so a sample it never
		 * receives leaves it believing the finger is still down.
		 *
		 * With the sheet up this is also the only path its controls
		 * have: the recognizer claims nothing there except pull-down,
		 * so slider drags and button taps pass straight through. */
		aster_ui_lvgl_pointer(tev[i].x, tev[i].y, pressed);
		if (g == ASTER_GEST_NONE) {
			continue;
		}
		/* Whatever the gesture turns into, it must not also land as a
		 * CLICKED on the widget the finger crossed. */
		lv_indev_wait_release(aster_ui_lvgl_indev());
		if (aster_qpanel_on_gesture(g, ASTER_QPANEL_OVER_LAUNCHER)) {
			/* One line per pull, same reasoning as the swipe log
			 * below: "the recognizer never fired" and "it fired and
			 * the sheet did not move" are different bugs in
			 * different files, and this is what tells them apart. */
			LOG_INF("launcher: pull-down (%d,%d) -> panel %s",
				(int)tev[i].x, (int)tev[i].y,
				aster_qpanel_is_open() ? "open" : "closed");
			continue;
		}
		if (g == ASTER_GEST_PAGE_NEXT || g == ASTER_GEST_PAGE_PREV) {
			aster_launcher_step_page(g == ASTER_GEST_PAGE_NEXT ? 1 : -1);
			LOG_INF("launcher: swipe %s (%d,%d) -> page %d",
				g == ASTER_GEST_PAGE_NEXT ? "next" : "prev",
				(int)tev[i].x, (int)tev[i].y, s_page);
		}
	}
	/* Drain the key-edge ring: at home there is no foreground app to
	 * deliver to (the exit key is a no-op here), and leaving edges
	 * queued would saturate the ring and leak into the next app. */
	{
		aster_key_evt_t kev[ASTER_PLAT_MAX_KEYS];

		while (aster_plat_input_keys_poll(kev, ASTER_PLAT_MAX_KEYS) > 0) {
		}
	}
	/* Drive the cpfont state machine BEFORE the first show: the first
	 * poll sweep is what discovers the baked files and starts the metric
	 * streaming on the sysworkq, so streaming() only becomes meaningful
	 * once it has run at least once. Also flips finished tables to READY
	 * (edge -> repaint once the tree exists). */
	if (aster_ui_fonts_poll()) {
		lv_obj_invalidate(aster_ui_lvgl_screen());
	}
	/* Boot request / first show: draw home on this UI-thread pass, but
	 * hold it while cpfont metric tables are still streaming: building
	 * the tree now would latch cold fonts into every label and the first
	 * flush would block lv_timer_handler walking glyphs over SD -- which
	 * also starves the first `aster app start` past its 15 s worker-ready
	 * timeout (observed as `worker wedged`). The poll above keeps
	 * streaming/flip progressing while we hold.
	 *
	 * Bounded to 12 s. Past it the home is built anyway, and with one
	 * face that is a blank screen plus one LOG_ERR from aster_ui_font()
	 * naming the reason -- the timeout no longer falls back to a second
	 * rasterizer. This whole wait is ~8x shorter than it was with eight
	 * faces to stream; see docs/cpfont-notes.md. */
	if (!s_visible && s_visible_want) {
		uint64_t now = (uint64_t)k_uptime_get();

		if (s_show_hold_ms == 0) {
			s_show_hold_ms = now ? now : 1;
		}
		if (aster_font_sd_streaming() &&
		    (now - s_show_hold_ms) < 12000u) {
			/* Font-free loading animation on the black panel
			 * (progress = how far the metric streams have got). */
			aster_splash_boot();
			aster_splash_tick();
			return;
		}
		aster_splash_off();
		aster_launcher_show();
		return;
	}
	/* The window moved under a tree that was placed for the old one. That is
	 * a rebuild, not a repaint: every tile, both home blocks and the page
	 * dots are positioned for a rectangle that is no longer there, and
	 * nothing in LVGL notices -- a full repaint of the old layout into the
	 * new window is exactly the "coherent picture of the wrong thing" the
	 * resync's own comment warns about.
	 *
	 * One pass of lag, and it is structural: the resync that makes
	 * aster_ui_lvgl_width() new runs in aster_ui_lvgl_pump() at the bottom of
	 * this function, so this sees the turn on the pass AFTER the one that
	 * took it. There is nothing to fix -- the panel has already moved and
	 * the glass is showing the old frame reinterpreted for one frame either
	 * way; what matters is that it converges, and it does, in the same
	 * second. */
	if (s_visible && (s_geom_w != (int)aster_ui_lvgl_width() ||
			  s_geom_h != (int)aster_ui_lvgl_height())) {
		LOG_INF("launcher: window %dx%d -> %dx%d, rebuilding", s_geom_w,
			s_geom_h, (int)aster_ui_lvgl_width(),
			(int)aster_ui_lvgl_height());
		s_dirty = true;
	}
	if (s_dirty) {
		/* install/uninstall / theme cycle: names + subset may change */
		if (s_visible) {
			aster_launcher_hide();
			aster_launcher_show();
		}
		return;
	}
	{
		int want = s_want_page;

		if (want >= 0) {
			s_want_page = -1; /* debug switch: visual only, no kv */
			set_page_visual(want);
		}
	}
	refresh_home();
	/* The quick-settings sheet's live rows (WiFi, SD). Driven from here and
	 * not from refresh_home() because this is the UI pump's own pass, not
	 * "the home page redrew" -- refresh_home bails early whenever the home
	 * label is gone, and the sheet outlives that. Self-throttled to 1 Hz
	 * inside; see aster_qpanel_tick(). */
	aster_qpanel_tick();
	aster_ui_lvgl_pump();
}

/* Registry change (install/uninstall): drawer names are new strings ->
 * rebuild rows. tiny_ttf grows its glyph cache automatically. */
void aster_launcher_on_registry_change(void)
{
	s_dirty = true;
}

/* Thread-safe page switch for the debug screenshot CLI: the shell only
 * records the wanted page; the pump consumes it on the UI thread. */
void aster_launcher_show_page(int page)
{
	s_want_page = page ? 1 : 0;
}

/* Swipe entry (UI thread). Steps the page and persists, the same way the
 * page dots do -- a swipe is the user changing where home is, not a
 * debug poke. There are two pages and no wrap: swiping left on the drawer
 * or right on the home page does nothing rather than cycling, which is
 * what the dots already show. */
void aster_launcher_step_page(int dir)
{
	int want = s_page + (dir > 0 ? 1 : -1);

	if (!s_home || !s_drawer || want < 0 || want > 1) {
		return;
	}
	go_page(want);
}
