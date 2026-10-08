#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * Screen-grab debug transport. The pixels already on the glass (or the
 * engine's last frame) are zlib-compressed into a temporary PSRAM block,
 * streamed as base64, then released. No screenshot buffer stays resident.
 *
 *   "SHOT hdr WxH zlib LEN" | "SHOT hdr WxH raw LEN"
 *   ceil(b64/640) x "SHOT b64 <idx> <640 b64 chars>"
 *   "SHOT end <crc32-hex>" | "SHOT err <why>"
 * CRC covers the bytes on the wire. Host: sdk/tools/screenshot.py
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
#include "aster_mem.h"
#include "aster_platform.h"
#include "ui_gfx.h"
#include "vendor/miniz.h"

LOG_MODULE_REGISTER(aster_shot, LOG_LEVEL_INF);

/* One output chunk per shell line: 480 B -> 640 B64 chars. */
/* Compressed UI frames land well under this. A noisy frame that does not
 * fit is sent raw from the buffer that already holds it. */
#define SHOT_ZLIB_CAP (128u * 1024u)
#define SHOT_ROW_MAX  (480u * 2u)
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

/* zlib of little-endian RGB565, streamed a row at a time so a panel-order
 * frame never needs a second full copy. Returns 0 and a PSRAM block the
 * caller frees, or -ENOSPC when the frame does not shrink into the cap. */
static int compress_frame(const uint8_t *frame, uint32_t w, uint32_t h,
			  uint32_t pitch, bool panel_order, uint8_t **out,
			  size_t *out_len)
{
	const size_t row_bytes = (size_t)w * 2u;
	tdefl_compressor *comp;
	uint8_t *zbuf;
	uint8_t row[SHOT_ROW_MAX];
	size_t produced = 0;
	tdefl_status status = TDEFL_STATUS_OKAY;

	if (row_bytes == 0 || row_bytes > SHOT_ROW_MAX || h == 0) {
		return -EINVAL;
	}
	comp = aster_psram_alloc(sizeof(*comp));
	zbuf = aster_psram_alloc(SHOT_ZLIB_CAP);
	if (!comp || !zbuf) {
		aster_psram_free(comp);
		aster_psram_free(zbuf);
		return -ENOMEM;
	}
	tdefl_init(comp, NULL, NULL,
		   (int)tdefl_create_comp_flags_from_zip_params(MZ_BEST_SPEED, 15, MZ_RLE));
	for (uint32_t y = 0; y < h && status == TDEFL_STATUS_OKAY; y++) {
		const uint8_t *src_row = frame + (size_t)y * pitch * 2u;
		const uint8_t *in = src_row;
		size_t left = row_bytes;
		const bool last = y + 1u == h;

		if (panel_order) {
			for (uint32_t x = 0; x < w; x++) {
				row[x * 2u] = src_row[x * 2u + 1u];
				row[x * 2u + 1u] = src_row[x * 2u];
			}
			in = row;
		} else if (pitch != w) {
			memcpy(row, src_row, row_bytes);
			in = row;
		}
		while (left > 0 || last) {
			size_t in_n = left;
			size_t out_n = SHOT_ZLIB_CAP - produced;
			const tdefl_flush flush = (last && in_n == left) ? TDEFL_FINISH
									  : TDEFL_NO_FLUSH;

			if (out_n == 0) {
				status = TDEFL_STATUS_PUT_BUF_FAILED;
				break;
			}
			status = tdefl_compress(comp, in, &in_n, zbuf + produced, &out_n, flush);
			in += in_n;
			left -= in_n;
			produced += out_n;
			if (status == TDEFL_STATUS_DONE) {
				break;
			}
			if (status != TDEFL_STATUS_OKAY || (in_n == 0 && out_n == 0)) {
				status = TDEFL_STATUS_PUT_BUF_FAILED;
				break;
			}
			if (!last && left == 0) {
				break;
			}
		}
	}
	aster_psram_free(comp);
	if (status != TDEFL_STATUS_DONE || produced == 0) {
		aster_psram_free(zbuf);
		return -ENOSPC;
	}
	*out = zbuf;
	*out_len = produced;
	return 0;
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
	uint8_t *zbuf = NULL;
	const uint8_t *src = NULL;
	const uint8_t *stream = NULL;
	uint32_t w = 0, h = 0, pitch = 0;
	size_t raw_bytes = 0;
	size_t stream_len = 0;
	uint32_t crc = 0;
	bool swap_rows = false; /* raw emit still has to swap panel order */
	bool zlib_stream = false;
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
			src = gb;
			swap_rows = true;
			goto pack;
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
			src = gb;
			swap_rows = frame_be;
			goto pack;
		}
	}

	fail(sh, "no-frame");
	goto out;

pack:
	if (compress_frame(src, w, h, pitch, swap_rows, &zbuf, &stream_len) == 0) {
		stream = zbuf;
		zlib_stream = true;
		swap_rows = false;
		LOG_INF("screenshot zlib %u -> %u", (unsigned)raw_bytes,
			(unsigned)stream_len);
	} else {
		aster_psram_free(zbuf);
		zbuf = NULL;
		if (pitch != w) {
			fail(sh, "compress");
			goto out;
		}
		stream = src;
		stream_len = raw_bytes;
		LOG_INF("screenshot raw %u", (unsigned)raw_bytes);
	}

	shell_fprintf(sh, SHELL_NORMAL, "SHOT hdr %ux%u %s %u\n",
		      (unsigned)w, (unsigned)h, zlib_stream ? "zlib" : "raw",
		      (unsigned)stream_len);

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
	while (pos < stream_len) {
		size_t n = stream_len - pos;
		const uint8_t *p;

		if (n > CH) {
			n = CH;
		}
		if (swap_rows) {
			for (size_t i = 0; i < n; i += 2u) {
				scratch[i] = stream[pos + i + 1u];
				scratch[i + 1u] = stream[pos + i];
			}
			p = scratch;
		} else {
			p = stream + pos;
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
	aster_plat_display_glass_drop();
	aster_psram_free(zbuf);
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
