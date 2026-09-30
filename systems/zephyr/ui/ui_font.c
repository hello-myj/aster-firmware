/* SPDX-License-Identifier: MIT
 * UI font router for the LVGL runtime.
 *
 * One backend: cpfont (font_sd.c), pre-baked 4bpp bitmaps per baked size
 * (ASTER_FONT_BAKED_PX). Metrics live in PSRAM and the bitmaps stream in
 * asynchronously, so the first frame never blocks on SD. aster_ui_font(px)
 * clamps any requested size to the nearest baked one -- there is no size it
 * refuses to answer, because there is no longer a second backend to answer
 * for it.
 *
 * There used to be a second backend -- an lv_tiny_ttf instance per pixel
 * size, all reading one ui.ttf off the SD card. It is gone, and the cost it
 * carried is the reason: eight resident metric tables and their glyph
 * caches were the single largest consumer of the PSRAM heap, with no
 * release path.
 *
 * What it also did, and what is worth knowing before "just add it back": a
 * rasterizer on hand means every codepoint outside the bake still draws. So
 * a bake that quietly shrank, or a card that lost a file, looked like a
 * font with a few odd characters rather than like a hole. With one source,
 * coverage is the whole story -- a face that is not READY now draws
 * nothing, which is why the state is logged instead of being inferred
 * from a page that merely looks thin.
 */
#include "ui_font.h"
#include "font_sd.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aster_uifont, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ *
 * Placeholder: used as LV_FONT_DEFAULT (lv_conf.h) and whenever no
 * baked face can answer. Zero glyphs resolve -> nothing draws.
 * ------------------------------------------------------------------ */

static bool placeholder_get_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
				uint32_t letter, uint32_t letter_next)
{
	(void)font;
	(void)dsc;
	(void)letter;
	(void)letter_next;
	return false;
}

static const void *placeholder_get_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *draw_buf)
{
	(void)g;
	(void)draw_buf;
	return NULL;
}

const lv_font_t aster_ui_placeholder_font = {
	.get_glyph_dsc = placeholder_get_dsc,
	.get_glyph_bitmap = placeholder_get_bitmap,
	.line_height = 8,
	.base_line = 0,
	.subpx = LV_FONT_SUBPX_NONE,
	.kerning = LV_FONT_KERNING_NONE,
	.underline_position = 0,
	.underline_thickness = 0,
	.dsc = NULL,
	.fallback = NULL,
	.user_data = NULL,
};

/* ------------------------------------------------------------------ *
 * Size routing                                                         *
 * ------------------------------------------------------------------ */

/* Nearest baked size, ALWAYS -- there is no "unbaked" answer any more.
 *
 * This used to return -1 outside a +/-4 px window, and that was correct
 * while a rasterizer sat behind it: an unbaked size still drew, from
 * tiny_ttf, and a caller asking for one was served. With a single face the
 * window is the whole question -- 12 and 14, which every app in the tree
 * names, would fall outside it and get nothing at all. Hard-clamping instead
 * means they draw at 20 px: text that overflows rather than text that
 * vanishes. That is a silent failure mode, but it is the one to prefer, and
 * it is the same rule the guest-side table applies (aster_text_snap).
 *
 * The list comes from font_sd (generated from the bake record), never a copy
 * kept here: a stale copy would snap callers onto a size with no .cpfont
 * behind it and draw blanks. */
static int cp_snap_px(uint8_t px)
{
	int best = -1;
	int pick = 0;

	for (uint32_t i = 0; i < aster_font_sd_face_n(); i++) {
		uint8_t baked = aster_font_sd_face_px(i);
		int d = (int)baked - (int)px;

		if (d < 0) {
			d = -d;
		}
		if (best < 0 || d < best) {
			best = d;
			pick = baked;
		}
	}
	return pick;
}

const lv_font_t *aster_ui_font(uint8_t px)
{
	/* px == 0 is the ABI's "whoever the UI uses" sentinel. It lives here,
	 * at the one place every caller funnels through, rather than at each
	 * call site: it used to be a literal in op_text only, so
	 * aster_gfx_text_measure(0, ...) measured the placeholder (width 0) while
	 * op_text(px=0) drew at the default size -- the same call disagreeing
	 * with itself about how wide the text is. */
	const lv_font_t *f;
	int snap;

	if (px == 0u) {
		px = ASTER_FONT_UI_PX;
	}
	snap = cp_snap_px(px);
	f = snap > 0 ? aster_font_sd_get((uint8_t)snap) : NULL;
	if (!f) {
		/* Reaching here no longer means "unbaked size" -- the clamp above
		 * covers every size. It means the face exists in the bake but is
		 * not drawable: no bake record at all, the .cpfont is missing
		 * from the card, or it has not finished streaming its metrics.
		 * So this is a real fault and worth one loud line -- with the
		 * rasterizer gone, nothing else covers for it, and a blank page
		 * with no log is the hardest kind of report to read. */
		static bool warned;

		if (!warned) {
			warned = true;
			LOG_ERR("no drawable face for %upx (nearest baked %dpx): "
				"SD missing/not mounted, ui_*.cpfont absent, or still "
				"streaming; text will be blank",
				(unsigned)px, snap);
		}
		return &aster_ui_placeholder_font;
	}
	return f;
}

/* ------------------------------------------------------------------ *
 * Recovery poll + async staging commit (UI thread)                     *
 * ------------------------------------------------------------------ */

int aster_ui_fonts_advance(void)
{
	int edge = 0;

	/* cpfont: state machine + staging commit (bitmaps that landed on
	 * the worker show up with one repaint) */
	if (aster_font_sd_poll()) {
		edge = 1;
	}
	if (aster_font_sd_commit()) {
		edge = 1;
	}
	/* Guest drawing can commit and acknowledge shared glyphs before this
	 * service runs. Native widgets must still observe that update. */
	static uint32_t observed_generation;
	uint32_t generation = aster_font_sd_generation();

	if (generation != observed_generation) {
		observed_generation = generation;
		edge = 1;
	}
	return edge;
}

int aster_ui_fonts_poll(void)
{
	int edge = aster_ui_fonts_advance();

	/* whatever landed is in the repaint this edge triggers, so retire the
	 * guest-facing request (see the note on repaint_ack) */
	aster_font_sd_repaint_ack();
	return edge;
}

/* Pre-warm the baked face for a requested size: snap to the nearest
 * baked slot and queue its bitmaps for async burst fetch. Never blocks,
 * drops silently when the queue is full. */
void aster_ui_font_warm(uint8_t px, const char *utf8)
{
	int snap = cp_snap_px(px ? px : ASTER_FONT_UI_PX);

	if (snap > 0) {
		aster_font_sd_warm((uint8_t)snap, utf8);
	}
}
