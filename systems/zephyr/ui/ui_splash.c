/* SPDX-License-Identifier: MIT
 * Boot splash: painted while the launcher holds its first home frame (the
 * cpfont metric tables are still streaming -- exactly when no text font is
 * usable yet), so the user never stares at panel noise or a dead black
 * screen. Deliberately font-free: everything is block pixels (hand-coded
 * 5x7/3x5 glyphs), because the whole wait is for fonts to become ready.
 *
 * Paint = one current-window-width x 96 RGB565 band pushed straight through
 * aster_plat_display_write_at at ~8 fps; costs nothing from the LVGL pool
 * and cannot be blocked by the font state machine it reports.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ui_splash.h"
#include "aster_mem.h"
#include "aster_platform.h"
#include "font_sd.h"

LOG_MODULE_REGISTER(aster_splash, LOG_LEVEL_INF);

/* The panel starts at its DT size (320x240 on the reference board), then
 * boot switches it to portrait (240x320). The splash is painted after that
 * switch, so compile-time DT dimensions cannot describe its window. */
#define SPL_H    96
#define SPL_PERIOD 125u	/* repaint at ~8 fps: enough motion, cheap SPI */

/* ASTER wordmark, 5x7, x3 scale, centred. */
static const uint8_t GLYPH_M[7] = { 0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11 };
static const uint8_t GLYPH_J[7] = { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C };
static const uint8_t GLYPH_X[7] = { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 };

/* decimal digits + %, 3x5, x2 scale */
static const uint8_t DIGITS[10][5] = {
	{ 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 },
	{ 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 },
	{ 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 },
	{ 7, 5, 7, 1, 7 },
};
static const uint8_t GLYPH_PCT[5] = { 5, 1, 2, 4, 5 };

/* spinner: 8 dots on a circle, one lit head + fading tail */
static const int8_t SPIN_R = 14;

static uint16_t *s_band;		/* PSRAM, W*H, native RGB565 */
static uint32_t s_w, s_y0;
static bool s_on;
static uint32_t s_next_ms;
static uint32_t s_phase;

static void px(int x, int y, uint16_t c)
{
	if ((unsigned)x < s_w && (unsigned)y < SPL_H) {
		s_band[(size_t)y * s_w + x] = c;
	}
}

static void blit(const uint8_t *rows, int nr, int bitsof, int scale,
		 int x0, int y0, uint16_t c)
{
	for (int r = 0; r < nr; r++) {
		for (int b = bitsof - 1; b >= 0; b--) {
			if (!(rows[r] & (1 << b))) {
				continue;
			}
			for (int sy = 0; sy < scale; sy++) {
				for (int sx = 0; sx < scale; sx++) {
					px(x0 + (bitsof - 1 - b) * scale + sx,
					   y0 + r * scale + sy, c);
				}
			}
		}
	}
}

static void draw_num(int x, int y, uint32_t v, uint16_t c)
{
	char tmp[4];
	int n = 0;

	if (v == 0u) {
		tmp[n++] = '0';
	}
	while (v > 0u && n < 3) {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	for (int i = n - 1; i >= 0; i--) {
		blit(DIGITS[tmp[i] - '0'], 5, 3, 2, x, y, c);
		x += 8;
	}
}

static void paint(uint32_t prog)
{
	const uint16_t COL = 0xFFFFu, BG = 0x0000u;
	int i;

	memset(s_band, 0, (size_t)s_w * SPL_H * sizeof(*s_band));

	/* wordmark ASTER, 15x21 each, 8 px gaps, centred near the top */
	{
		int total = 15 * 3 + 8 * 2;
		int lx = ((int)s_w - total) / 2;
		int ly = 8;

		blit(GLYPH_M, 7, 5, 3, lx, ly, COL);
		blit(GLYPH_J, 7, 5, 3, lx + 23, ly, COL);
		blit(GLYPH_X, 7, 5, 3, lx + 46, ly, COL);
		/* spinner ring right of the wordmark */
		for (i = 0; i < 8; i++) {
			static const int8_t dx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
			static const int8_t dy[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
			int a = (int)((s_phase + (uint32_t)i) % 8u);
			int cx = lx + total + 18 + dx[a] * SPIN_R;
			int cy = ly + 10 + dy[a] * SPIN_R;
			int rad = (i == 0) ? 3 : 2;

			for (int yy = -rad; yy <= rad; yy++) {
				for (int xx = -rad; xx <= rad; xx++) {
					if (xx * xx + yy * yy <= rad * rad) {
						px(cx + xx, cy + yy, COL);
					}
				}
			}
		}
	}

	/* progress bar: 160x8 frame, filled by prog% */
	{
		int bx = ((int)s_w - 160) / 2, by = 46;

		for (int x = 0; x < 160; x++) {
			px(bx + x, by, COL);
			px(bx + x, by + 7, COL);
		}
		for (int y = 0; y < 8; y++) {
			px(bx, by + y, COL);
			px(bx + 159, by + y, COL);
		}
		int fill = (160 - 4) * (int)prog / 100;

		for (int y = by + 2; y <= by + 5; y++) {
			for (int x = 0; x < fill; x++) {
				px(bx + 2 + x, y, COL);
			}
		}

		/* percent digits under the bar */
		draw_num((int)s_w / 2 - 16, 62, prog, COL);
		blit(GLYPH_PCT, 5, 3, 2, (int)s_w / 2 + (prog >= 100 ? 14 : 8), 62,
		     COL);
	}
	ARG_UNUSED(BG);

	int err = aster_plat_display_write_at((const uint8_t *)s_band, 0, s_y0,
					     s_w, SPL_H, s_w);

	if (err) {
		LOG_ERR("splash write %ux%u at y=%u failed: %d",
			(unsigned)s_w, SPL_H, (unsigned)s_y0, err);
	} else if (s_phase == 1u) {
		LOG_INF("splash first frame %ux%u at y=%u",
			(unsigned)s_w, SPL_H, (unsigned)s_y0);
	}
}

void aster_splash_boot(void)
{
	aster_display_info_t di = { .size = sizeof(di), .version = 1 };

	if (s_on) {
		return;
	}
	if (aster_plat_display_get_info(0, &di) != ASTER_OK ||
	    di.width < 160u || di.height < SPL_H) {
		LOG_ERR("splash: invalid display window %ux%u",
			(unsigned)di.width, (unsigned)di.height);
		return;
	}
	s_w = di.width;
	s_y0 = (di.height - SPL_H) / 2u;
	s_band = aster_psram_alloc((size_t)s_w * SPL_H * sizeof(*s_band));
	if (!s_band) {
		LOG_ERR("splash: band allocation failed");
		return;
	}
	s_on = true;
	s_phase = 0;
	s_next_ms = 0;
	aster_splash_tick();
}

void aster_splash_tick(void)
{
	uint32_t now = (uint32_t)k_uptime_get_32();

	if (!s_on || (int32_t)(now - s_next_ms) < 0) {
		return;
	}
	s_next_ms = now + SPL_PERIOD;
	s_phase++;
	paint(aster_font_sd_progress());
	(void)aster_plat_display_flush_drain();
}

void aster_splash_off(void)
{
	if (!s_on) {
		return;
	}
	aster_psram_free(s_band);
	s_band = NULL;
	s_w = s_y0 = 0;
	s_on = false;
}
