#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * Screen-grab debug transport: snapshot the active LVGL screen and stream
 * it out as base64 lines on the shell (see ui_screenshot.h).
 *
 * Flow (PSRAM transient, freed the moment the grab is out):
 *   lv_snapshot_take(RGB565) -> rows repacked (stride stripped) to raw[w*h*2]
 *   "SHOT hdr WxH raw LEN"
 *   ceil(b64/640) x "SHOT b64 <idx> <640 b64 chars>"   (idx zero-padded)
 *   "SHOT end <crc32-hex>" | "SHOT err <why>"
 * Host side: sdk/tools/shell_shot.py reassembles -> PNG.
 *
 * v1 ships raw (no codec): the vendored miniz is compiled inflate-only for
 * PNG decode, and pulling in deflate would collide. A 135x240 RGB565 frame
 * is 65 KB -> ~86 KB base64 -> ~7.5 s at 115200, acceptable for a debug
 * tool. Deflate (RLE crushes flat UI) is a clean follow-up.
 */
#include "ui_screenshot.h"

#if IS_ENABLED(CONFIG_ASTER_SCREENSHOT)

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/crc.h>
#include <lvgl.h>
#include "aster_mem.h"
#include "aster_platform.h"
#include "ui_gfx.h"
#include "ui_lvgl.h"

LOG_MODULE_REGISTER(aster_shot, LOG_LEVEL_INF);

/* The last-resort source below allocates a whole RGB565 screen out of the
 * LVGL pool (LV_MEM_SIZE), CONTIGUOUS. It only runs when the glass record
 * and the engine's last frame have both missed, which is exactly the
 * situation where a silent failure is least affordable -- so pin the
 * relationship to LV_MEM_SIZE here, where a shrink of the pool breaks the
 * build instead of quietly removing the safety net. (S6 cut that pool from
 * 512 KB to 224 KB on a measured 22,584 B peak; this is what keeps the next
 * cut from taking the floor with it.) Rotation swaps width and height, so
 * the product is the invariant that matters. */
#define SHOT_SNAP_BYTES ((size_t)DT_PROP(DT_CHOSEN(zephyr_display), width) *  \
			 (size_t)DT_PROP(DT_CHOSEN(zephyr_display), height) * 2u)
#define SHOT_POOL_HEADROOM (48u * 1024u)
BUILD_ASSERT(SHOT_SNAP_BYTES + SHOT_POOL_HEADROOM <= (size_t)LV_MEM_SIZE,
	     "LV_MEM_SIZE cannot hold a full-screen RGB565 snapshot plus "
	     "headroom: ui_screenshot.c's last-resort source would fail at the "
	     "one moment it exists for");

/* One output chunk per shell line: 480 B raw -> 640 B64 chars. */
#define SHOT_B64_PER_LINE 640
#define SHOT_CHUNK_BYTES  (SHOT_B64_PER_LINE / 4 * 3) /* 480 */

static struct k_sem s_req = Z_SEM_INITIALIZER(s_req, 0, 1);   /* shell -> pump */
static struct k_sem s_done = Z_SEM_INITIALIZER(s_done, 0, 1); /* pump -> shell */
static const struct shell *s_sh;
static volatile bool s_armed; /* pump: grab now; shell: wait for done */

static char b64_char(unsigned v)
{
	return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[v & 63];
}

/* Encode exactly `len` bytes (multiple of 3) with no padding. */
static void b64_chunk(char *dst, const uint8_t *src, size_t len)
{
	size_t o = 0;

	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];

		dst[o++] = b64_char(v >> 18);
		dst[o++] = b64_char(v >> 12);
		dst[o++] = b64_char(v >> 6);
		dst[o++] = b64_char(v);
	}
}

static void fail(const struct shell *sh, const char *why)
{
	shell_fprintf(sh, SHELL_NORMAL, "SHOT err %s\n", why);
}

/* Copy a captured frame into a fresh ABI-order RGB565 buffer the PNG
 * encoder can read: strip row padding and undo panel byte order when the
 * source is panel-ordered (`panel_order`).
 *
 * Kept in one place on purpose. The PNG decodes whatever bytes it is
 * handed as little-endian RGB565, so a wrong (or missing) swap here shows
 * up as a colour-swapped screen-grab, and an unswapped panel frame is
 * indistinguishable from a genuinely wrong render. */
static bool repack_abi(const struct shell *sh, const uint8_t *frame, void **raw_out,
		       uint32_t w, uint32_t h, uint32_t pitch, bool panel_order)
{
	uint8_t *raw = aster_psram_alloc((size_t)w * h * 2u);

	if (!raw) {
		fail(sh, "psram");
		return false;
	}
	for (uint32_t y = 0; y < h; y++) {
		const uint16_t *s16 = (const uint16_t *)(frame + (size_t)y * pitch * 2u);
		uint16_t *d16 = (uint16_t *)(raw + (size_t)y * w * 2u);

		if (!panel_order) {
			memcpy(d16, s16, (size_t)w * 2u);
			continue;
		}
		for (uint32_t x = 0; x < w; x++) {
			d16[x] = (uint16_t)((s16[x] << 8) | (s16[x] >> 8));
		}
	}
	*raw_out = raw;
	return true;
}

/* Runs on the UI/worker thread, between lv_timer_handler ticks. */
void aster_screenshot_poll(void)
{
	if (!k_sem_take(&s_req, K_NO_WAIT)) {
		s_armed = true;
	}
	if (!s_armed) {
		return;
	}
	s_armed = false;

	const struct shell *sh = s_sh;
	lv_obj_t *scr = aster_ui_lvgl_screen();
	lv_draw_buf_t *snap = NULL;
	void *raw = NULL;
	const uint8_t *src = NULL;
	uint32_t w = 0, h = 0, pitch = 0;
	size_t raw_bytes = 0;
	uint32_t crc = 0;
	bool swap_rows = false; /* source is panel-order: swap while emitting */
	int rc = -EIO;

	/* Prefer the display's record of what was handed to the wire. It is
	 * the ONLY source downstream of the present path, i.e. the only one a
	 * byte-order bug in that path can show up in -- this is exactly the
	 * blind spot that let the flip-path SWAR ship a swapped frame while
	 * every grab looked fine (see the probe in display.c). It also covers
	 * the apps the other two sources cannot: the v1 inline path
	 * (game.2048, gallery) presents its own LE surface and touches neither
	 * the engine nor LVGL, and a band UI (LVGL) is composited into the
	 * record band by band. Stored panel-order, hence the repack. */
	{
		const uint8_t *gb = NULL;

		if (aster_plat_display_glass_frame(&gb, &w, &h, &pitch)) {
			raw_bytes = (size_t)w * h * 2u;
			swap_rows = (pitch == w);
			if (swap_rows) {
				src = gb;
			} else if (repack_abi(sh, gb, &raw, w, h, pitch, true)) {
				src = raw;
			} else {
				goto out;
			}
			goto stream;
		}
	}

	/* Headless (no panel -> no record). The gfx engine's last rasterized
	 * surface is the app-side buffer, i.e. the pixels BEFORE the present
	 * path ran: good enough when there is no present path to speak of,
	 * and it is ABI RGB565 already, so it can stream without a repack. */
	{
		const uint8_t *gb = NULL;
		bool frame_be = false;

		if (aster_gfx_last_frame(&gb, &w, &h, &pitch, &frame_be)) {
			raw_bytes = (size_t)w * h * 2u;
			if (pitch == w && !frame_be) {
				src = gb;
			} else if (repack_abi(sh, gb, &raw, w, h, pitch, frame_be)) {
				src = raw;
			} else {
				goto out;
			}
			goto stream;
		}
	}

	if (!scr) {
		fail(sh, "no-ui");
		goto out;
	}

	snap = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
	if (!snap) {
		fail(sh, "snapshot");
		goto out;
	}

	w = snap->header.w;
	h = snap->header.h;
	raw_bytes = (size_t)w * h * 2u;

	/* Rows already packed (our 240px RGB565 panel: stride == w*2): encode
	 * straight from the snapshot buffer. Repacking into a second ~150 KB
	 * contiguous PSRAM block was what made filer/reader grabs fail with
	 * `psram` once the shared heap got busy. */
	if (snap->header.stride == (size_t)w * 2u) {
		src = snap->data;
	} else {
		raw = aster_psram_alloc(raw_bytes);
		if (!raw) {
			fail(sh, "psram");
			goto out;
		}
		/* repack: strip stride padding so the host reads packed rows */
		for (uint32_t y = 0; y < h; y++) {
			memcpy((uint8_t *)raw + (size_t)y * w * 2u,
			       (const uint8_t *)snap->data +
				       (size_t)y * snap->header.stride,
			       (size_t)w * 2u);
		}
		src = raw;
		lv_draw_buf_destroy(snap);
		snap = NULL;
	}

stream:
	shell_fprintf(sh, SHELL_NORMAL, "SHOT hdr %ux%u raw %u\n",
		      (unsigned)w, (unsigned)h, (unsigned)raw_bytes);

	/* Chunk the SOURCE in 480-byte groups (multiple of 3), so only the
	 * final group can carry an '=' pad tail. 480 raw -> 640 b64 chars.
	 *
	 * Both the swap and the CRC ride along on the emit. Swapping the whole
	 * frame into a second buffer first is what this used to do, and that
	 * ~150 KB contiguous PSRAM allocation is exactly what made grabs fail
	 * with `psram` once the shared heap got busy -- see the note on the
	 * snapshot path below. Every chunk is an even byte count (480 and the
	 * even total), so the 16-bit swap never straddles a chunk boundary,
	 * and CRC-ing the swapped stream chunk by chunk gives the same value
	 * as CRC-ing the whole repacked frame did. */
	const size_t CH = SHOT_CHUNK_BYTES;
	static char line[SHOT_B64_PER_LINE + 8];
	static uint8_t scratch[SHOT_CHUNK_BYTES];
	size_t pos = 0;
	unsigned idx = 0;

	crc = 0;
	while (pos < raw_bytes) {
		size_t n = raw_bytes - pos;
		const uint8_t *p;

		if (n > CH) {
			n = CH;
		}
		if (swap_rows) {
			for (size_t i = 0; i < n; i += 2u) {
				scratch[i] = src[pos + i + 1u];
				scratch[i + 1u] = src[pos + i];
			}
			p = scratch;
		} else {
			p = src + pos;
		}
		crc = crc32_ieee_update(crc, p, n);
		size_t whole = (n / 3u) * 3u;
		size_t rem = n - whole; /* 0,1,2 -- only ever on the last chunk */

		b64_chunk(line, p, whole);
		size_t o = whole / 3u * 4u;

		if (rem) {
			uint32_t v = (uint32_t)p[whole] << 16;

			if (rem > 1) {
				v |= (uint32_t)p[whole + 1] << 8;
			}
			line[o++] = b64_char(v >> 18);
			line[o++] = b64_char(v >> 12);
			line[o++] = rem > 1 ? b64_char(v >> 6) : '=';
			line[o++] = '=';
		}
		line[o] = 0;
		shell_fprintf(sh, SHELL_NORMAL, "SHOT b64 %04u %s\n", idx++, line);
		pos += n;
	}

	shell_fprintf(sh, SHELL_NORMAL, "SHOT end %08x\n", crc);
	rc = 0;

out:
	if (snap) {
		lv_draw_buf_destroy(snap); /* fast path: src aliased its data */
	}
	if (raw) {
		aster_psram_free(raw);
	}
	if (rc) {
		LOG_WRN("screenshot failed (%d)", rc);
	}
	k_sem_give(&s_done);
}

/* Blocks on the shell thread until the pump finishes one grab. */
int aster_screenshot_request(const struct shell *sh)
{
	static struct k_mutex gate = Z_MUTEX_INITIALIZER(gate);
	int err = 0;

	k_mutex_lock(&gate, K_FOREVER);
	s_sh = sh;
	k_sem_give(&s_req);
	if (k_sem_take(&s_done, K_SECONDS(180))) {
		err = -ETIMEDOUT;
	}
	k_mutex_unlock(&gate);
	return err;
}

#endif /* CONFIG_ASTER_SCREENSHOT */
