/* SPDX-License-Identifier: MIT
 * Shared native-size cpfont router. Full-coverage SD resources supply glyphs;
 * the loader allocates per-size LRU caches on demand and releases them when
 * ownership passes between desktop and APP. A requested size chooses the
 * nearest published face; rasterized glyph pixels are never resized here.
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

/* Firmware and SDK use the same generated face order and nearest-size rule. */
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
				"SD missing/not mounted, ui_*.cpfont absent, or no glyph cache memory; text will be blank",
				(unsigned)px, snap);
		}
		return &aster_ui_placeholder_font;
	}
	return f;
}

/* ------------------------------------------------------------------ *
 * Resource discovery and repaint generation (UI thread)                     *
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

/* Fetch cold glyphs before presentation; cache hits stay in PSRAM. */
void aster_ui_font_warm(uint8_t px, const char *utf8)
{
	int snap = cp_snap_px(px ? px : ASTER_FONT_UI_PX);

	if (snap > 0) {
		aster_font_sd_warm((uint8_t)snap, utf8);
	}
}

/* Prepare the common body/title sizes after hardware orientation is known.
 * Exact requested sizes still work independently; no glyph bitmap is scaled. */
static uint8_t screen_font_px(uint32_t role,uint32_t width,uint32_t height)
{
 uint64_t x=(uint64_t)width*role*256u/320u,y=(uint64_t)height*role*256u/480u;
 uint64_t request=MIN(x,y),best=UINT64_MAX;uint8_t pick=ASTER_FONT_UI_PX;
 for(uint32_t i=0;i<aster_font_sd_face_n();i++) {
  uint8_t px=aster_font_sd_face_px(i);uint64_t size=(uint64_t)px*256u;
  uint64_t d=size>request?size-request:request-size;
  if(d<best) {best=d;pick=px;}
 }
 return pick;
}
void aster_ui_fonts_prepare(uint32_t width,uint32_t height,const char *title)
{
 const char *common="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz .,:;!?+-/%()";
 uint8_t body=screen_font_px(16,width,height),heading=screen_font_px(20,width,height);
 (void)aster_font_sd_poll();
 (void)aster_ui_font(body);aster_ui_font_warm(body,common);
 (void)aster_ui_font(heading);aster_ui_font_warm(heading,title?title:"");
}
