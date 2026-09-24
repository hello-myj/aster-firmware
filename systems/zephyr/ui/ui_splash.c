/* SPDX-License-Identifier: MIT
 * Boot splash: painted while the launcher holds its first home frame (the
 * cpfont metric tables are still streaming -- exactly when no text font is
 * usable yet), so the user never stares at panel noise or a dead black
 * screen. Deliberately font-free: everything is block pixels (hand-coded
 * 5x7/3x5 glyphs), because the whole wait is for fonts to become ready.
 *
 * Paint = one logical-width x 96 RGB565 band pushed straight through
 * aster_plat_display_write_at at ~8 fps; costs nothing from the LVGL pool
 * and cannot be blocked by the font state machine it reports.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ui_splash.h"
#include "ui_geom.h"
#include "aster_mem.h"
#include "aster_platform.h"
#include "font_sd.h"

LOG_MODULE_REGISTER(aster_splash, LOG_LEVEL_INF);

/* The band IS the logical width, not a 240 px strip. It used to be the
 * literal 240 with a y of 104, which was a correct picture of one panel:
 * on a 320-wide screen write_at accepted the band happily and the remaining
 * 80 columns kept whatever the panel had -- panel noise, which is the exact
 * thing the splash exists to cover. Sizing to ASTER_UI_W fixes the coverage
 * and makes every `(SPL_W - w) / 2` below centre on the screen for free. */
#define SPL_W    ASTER_UI_W
#define SPL_H    96
#define SPL_Y0   ((ASTER_UI_H - SPL_H) / 2)	/* centred band */
#define SPL_PERIOD 125u	/* repaint at ~8 fps: enough motion, cheap SPI */

/* SPL_Y0 is unsigned arithmetic on a difference: a logical screen shorter
 * than the band wraps to a huge y and write_at drops the whole paint, which
 * would leave the splash invisible with no error anywhere. */
BUILD_ASSERT(ASTER_UI_H >= SPL_H, "logical screen is shorter than the splash band");

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
static bool s_on;
static uint32_t s_next_ms;
static uint32_t s_phase;

static void px(int x, int y, uint16_t c)
{
	if ((unsigned)x < SPL_W && (unsigned)y < SPL_H) {
		s_band[(size_t)y * SPL_W + x] = c;
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

	memset(s_band, 0, SPL_W * SPL_H * 2);

	/* wordmark ASTER, 15x21 each, 8 px gaps, centred near the top */
	{
		int total = 15 * 3 + 8 * 2;
		int lx = (SPL_W - total) / 2;
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
		int bx = (SPL_W - 160) / 2, by = 46;

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
		draw_num(SPL_W / 2 - 16, 62, prog, COL);
		blit(GLYPH_PCT, 5, 3, 2, SPL_W / 2 + (prog >= 100 ? 14 : 8), 62,
		     COL);
	}
	ARG_UNUSED(BG);

	(void)aster_plat_display_write_at((const uint8_t *)s_band, 0, SPL_Y0,
					SPL_W, SPL_H, SPL_W);
}

void aster_splash_boot(void)
{
	if (s_on) {
		return;
	}
	s_band = aster_psram_alloc(SPL_W * SPL_H * 2);
	if (!s_band) {
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
	s_on = false;
}
