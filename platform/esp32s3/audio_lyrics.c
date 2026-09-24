/* SPDX-License-Identifier: MIT
 * ID3 USLT/SYLT or a same-directory .lrc → UTF-8 blob in PSRAM.
 */
#include "audio_lyrics.h"
#include "aster_bytes.h"
#include "aster_mem.h"
#include "aster_storage.h"
#include "gb2312_map.h"

#include <errno.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aster_alrc, LOG_LEVEL_INF);

#define LYR_MAX 12288

static char *s_lyr;
static size_t s_lyr_n;

static int utf8_put(char *dst, int max, int *o, uint32_t cp)
{
	int k = *o;

	if (cp < 0x80) {
		if (k + 1 >= max) {
			return 0;
		}
		dst[k] = (char)cp;
		*o = k + 1;
		return 1;
	}
	if (cp < 0x800) {
		if (k + 2 >= max) {
			return 0;
		}
		dst[k] = (char)(0xC0 | (cp >> 6));
		dst[k + 1] = (char)(0x80 | (cp & 0x3F));
		*o = k + 2;
		return 1;
	}
	if (k + 3 >= max) {
		return 0;
	}
	dst[k] = (char)(0xE0 | (cp >> 12));
	dst[k + 1] = (char)(0x80 | ((cp >> 6) & 0x3F));
	dst[k + 2] = (char)(0x80 | (cp & 0x3F));
	*o = k + 3;
	return 1;
}

static int utf8_lead_len(uint8_t c)
{
	if (c < 0x80) {
		return 1;
	}
	if (c >= 0xC2 && c <= 0xDF) {
		return 2;
	}
	if (c >= 0xE0 && c <= 0xEF) {
		return 3;
	}
	if (c >= 0xF0 && c <= 0xF4) {
		return 4;
	}
	return 0;
}

static int looks_utf8(const uint8_t *p, int n)
{
	int i = 0;
	int bad = 0;

	while (i < n) {
		int k = utf8_lead_len(p[i]);
		int j;

		if (k == 1) {
			i++;
			continue;
		}
		if (k == 0) {
			bad++;
			i++;
			if (bad > 3) {
				return 0;
			}
			continue;
		}
		if (i + k > n) {
			break;
		}
		for (j = 1; j < k; j++) {
			if ((p[i + j] & 0xC0) != 0x80) {
				bad++;
				i++;
				if (bad > 3) {
					return 0;
				}
				k = 0;
				break;
			}
		}
		if (k) {
			i += k;
		}
	}
	return bad == 0;
}

static int gb2312_to_utf8(const uint8_t *src, int n, char *dst, int max)
{
	int i = 0;
	int o = 0;

	while (i < n && o + 1 < max) {
		uint8_t a = src[i];

		if (a < 0x80) {
			dst[o++] = (char)a;
			i++;
			continue;
		}
		if (i + 1 >= n) {
			break;
		}
		{
			uint8_t b = src[i + 1];
			int r = (int)a - 0xA1;
			int c = (int)b - 0xA1;
			uint32_t cp = 0;

			if (r >= 0 && r < 94 && c >= 0 && c < 94) {
				cp = gb2312_map[r * 94 + c];
			}
			i += 2;
			if (!cp) {
				cp = '.';
			}
			if (!utf8_put(dst, max, &o, cp)) {
				break;
			}
		}
	}
	if (o < max) {
		dst[o] = 0;
	}
	return o;
}

static int latin_to_utf8(const uint8_t *src, int n, char *dst, int max)
{
	int i;
	int o = 0;

	for (i = 0; i < n && o + 1 < max; i++) {
		if (!utf8_put(dst, max, &o, src[i])) {
			break;
		}
	}
	if (o < max) {
		dst[o] = 0;
	}
	return o;
}

static int utf16_to_utf8(const uint8_t *src, int n, int be, char *dst, int max)
{
	int i = 0;
	int o = 0;

	if (n >= 2 && ((src[0] == 0xFF && src[1] == 0xFE) ||
		       (src[0] == 0xFE && src[1] == 0xFF))) {
		be = src[0] == 0xFE;
		i = 2;
	}
	while (i + 1 < n && o + 1 < max) {
		uint32_t cp = be ? ((uint32_t)src[i] << 8) | src[i + 1]
				 : src[i] | ((uint32_t)src[i + 1] << 8);

		i += 2;
		if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) {
			uint32_t lo = be ? ((uint32_t)src[i] << 8) | src[i + 1]
					 : src[i] | ((uint32_t)src[i + 1] << 8);

			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				cp = 0x10000u + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				i += 2;
			}
		}
		if (cp == 0) {
			if (!utf8_put(dst, max, &o, '\n')) {
				break;
			}
			continue;
		}
		if (!utf8_put(dst, max, &o, cp)) {
			break;
		}
	}
	if (o < max) {
		dst[o] = 0;
	}
	return o;
}

static int id3_text_to_utf8(uint8_t enc, const uint8_t *src, int n, char *dst, int max)
{
	if (n <= 0 || max < 2) {
		return 0;
	}
	if (enc == 1) {
		return utf16_to_utf8(src, n, 0, dst, max);
	}
	if (enc == 2) {
		return utf16_to_utf8(src, n, 1, dst, max);
	}
	if (enc == 3 || looks_utf8(src, n)) {
		int c = n < max - 1 ? n : max - 1;

		memcpy(dst, src, (size_t)c);
		dst[c] = 0;
		return c;
	}
	if (enc == 0) {
		int gbk = 0;
		int i;

		for (i = 0; i + 1 < n; i++) {
			if (src[i] >= 0x81) {
				gbk = 1;
				break;
			}
		}
		if (gbk) {
			return gb2312_to_utf8(src, n, dst, max);
		}
		return latin_to_utf8(src, n, dst, max);
	}
	return gb2312_to_utf8(src, n, dst, max);
}

static int skip_id3_str(uint8_t enc, const uint8_t *p, int n)
{
	int i = 0;

	if (enc == 1 || enc == 2) {
		while (i + 1 < n && (p[i] | p[i + 1])) {
			i += 2;
		}
		return (i + 1 < n) ? i + 2 : n;
	}
	while (i < n && p[i]) {
		i++;
	}
	return (i < n) ? i + 1 : n;
}

static int store_utf8(const char *s, int n)
{
	if (n <= 0) {
		return 0;
	}
	aster_audio_lyrics_clear();
	s_lyr = aster_psram_alloc((size_t)n + 1u);
	if (!s_lyr) {
		return 0;
	}
	memcpy(s_lyr, s, (size_t)n);
	s_lyr[n] = 0;
	s_lyr_n = (size_t)n;
	return n;
}

static int decode_uslt(const uint8_t *body, int n)
{
	uint8_t enc;
	int off;
	char *tmp;
	int out;

	if (n < 5) {
		return 0;
	}
	enc = body[0];
	off = 4 + skip_id3_str(enc, body + 4, n - 4);
	if (off >= n) {
		return 0;
	}
	tmp = aster_psram_alloc(LYR_MAX);
	if (!tmp) {
		return 0;
	}
	out = id3_text_to_utf8(enc, body + off, n - off, tmp, LYR_MAX);
	if (out > 0) {
		store_utf8(tmp, out);
	}
	aster_psram_free(tmp);
	return out;
}

static int decode_sylt(const uint8_t *body, int n)
{
	uint8_t enc;
	uint8_t fmt;
	int off;
	char *tmp;
	int o = 0;

	if (n < 6) {
		return 0;
	}
	enc = body[0];
	fmt = body[4];
	off = 6 + skip_id3_str(enc, body + 6, n - 6);
	tmp = aster_psram_alloc(LYR_MAX);
	if (!tmp) {
		return 0;
	}
	while (off < n && o + 16 < LYR_MAX) {
		int t0 = off;
		int term = skip_id3_str(enc, body + off, n - off);
		int tn;
		uint32_t ms = 0;
		char line[192];
		int ln;

		off += term;
		if (off + 4 > n) {
			break;
		}
		ms = aster_be32(body + off);
		off += 4;
		if (fmt == 2) {
			unsigned mm = (ms / 60000u) % 100u;
			unsigned ss = (ms / 1000u) % 60u;
			unsigned xx = (ms / 10u) % 100u;
			int p = 0;

			line[p++] = '[';
			line[p++] = (char)('0' + mm / 10u);
			line[p++] = (char)('0' + mm % 10u);
			line[p++] = ':';
			line[p++] = (char)('0' + ss / 10u);
			line[p++] = (char)('0' + ss % 10u);
			line[p++] = '.';
			line[p++] = (char)('0' + xx / 10u);
			line[p++] = (char)('0' + xx % 10u);
			line[p++] = ']';
			ln = id3_text_to_utf8(enc, body + t0, term, line + p,
					      (int)sizeof(line) - p);
			if (ln < 0) {
				ln = 0;
			}
			tn = p + ln;
			if (tn > 0 && line[tn - 1] != '\n' && tn + 1 < (int)sizeof(line)) {
				line[tn++] = '\n';
			}
			if (o + tn < LYR_MAX) {
				memcpy(tmp + o, line, (size_t)tn);
				o += tn;
			}
		}
	}
	if (o > 0) {
		store_utf8(tmp, o);
	}
	aster_psram_free(tmp);
	return o;
}

static int id3_from_file(const char *path)
{
	struct fs_file_t f;
	uint8_t head[10];
	size_t tag;
	size_t pos;
	int v24;
	int n;
	int got = 0;

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) {
		return 0;
	}
	n = aster_fs_read_safe(&f, head, 10);
	if (n < 10 || head[0] != 'I' || head[1] != 'D' || head[2] != '3') {
		fs_close(&f);
		return 0;
	}
	tag = 10u + aster_synchsafe32(head + 6);
	if (head[5] & 0x10) {
		tag += 10u;
	}
	v24 = head[3] >= 4;
	pos = 10;
	if (head[5] & 0x40) {
		uint8_t eh[4];

		if (aster_fs_read_safe(&f, eh, 4) < 4) {
			fs_close(&f);
			return 0;
		}
		pos += v24 ? aster_synchsafe32(eh) : aster_be32(eh);
		if (pos > 14) {
			(void)fs_seek(&f, (off_t)(pos - 14), FS_SEEK_CUR);
		}
	}
	while (pos + 10 <= tag) {
		uint8_t h[10];
		uint32_t sz;
		int uslt;
		int sylt;

		if (fs_seek(&f, (off_t)pos, FS_SEEK_SET) != 0) {
			break;
		}
		if (aster_fs_read_safe(&f, h, 10) < 10 || h[0] == 0) {
			break;
		}
		sz = v24 ? aster_synchsafe32(h + 4) : aster_be32(h + 4);
		uslt = memcmp(h, "USLT", 4) == 0;
		sylt = memcmp(h, "SYLT", 4) == 0;
		if ((uslt || sylt) && sz > 4 && sz < 64u * 1024u) {
			size_t take = sz > (LYR_MAX + 64u) ? (LYR_MAX + 64u) : sz;
			uint8_t *body = aster_psram_alloc(take);

			if (body && aster_fs_read_safe(&f, body, take) == (int)take) {
				got = uslt ? decode_uslt(body, (int)take)
					   : decode_sylt(body, (int)take);
			}
			if (body) {
				aster_psram_free(body);
			}
			if (got > 0) {
				break;
			}
		}
		pos += 10u + sz;
	}
	fs_close(&f);
	return got;
}

static int load_lrc_file(const char *path)
{
	struct fs_dirent st;
	struct fs_file_t f;
	uint8_t *raw;
	char *utf;
	int n;
	int out = 0;

	if (fs_stat(path, &st) != 0 || st.type != FS_DIR_ENTRY_FILE || st.size < 4) {
		return 0;
	}
	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) {
		return 0;
	}
	n = (int)st.size;
	if (n > LYR_MAX) {
		n = LYR_MAX;
	}
	raw = aster_psram_alloc((size_t)n);
	utf = aster_psram_alloc(LYR_MAX);
	if (raw && utf) {
		int r = aster_fs_read_safe(&f, raw, (size_t)n);

		if (r > 0) {
			out = id3_text_to_utf8(0, raw, r, utf, LYR_MAX);
			if (out > 0) {
				store_utf8(utf, out);
			}
		}
	}
	if (raw) {
		aster_psram_free(raw);
	}
	if (utf) {
		aster_psram_free(utf);
	}
	fs_close(&f);
	return out;
}

static int sibling_lrc(const char *path)
{
	char alt[256];
	size_t n;
	size_t i;
	size_t dot = 0;

	if (!path) {
		return 0;
	}
	n = strlen(path);
	if (n < 5 || n + 1 >= sizeof(alt)) {
		return 0;
	}
	memcpy(alt, path, n + 1);
	for (i = 0; i < n; i++) {
		if (alt[i] == '/') {
			dot = 0;
		} else if (alt[i] == '.') {
			dot = i;
		}
	}
	if (!dot || n - dot > 5) {
		return 0;
	}
	alt[dot] = 0;
	if (dot + 4 >= sizeof(alt)) {
		return 0;
	}
	memcpy(alt + dot, ".lrc", 5);
	if (load_lrc_file(alt) > 0) {
		return 1;
	}
	memcpy(alt + dot, ".LRC", 5);
	return load_lrc_file(alt) > 0;
}

void aster_audio_lyrics_clear(void)
{
	if (s_lyr) {
		aster_psram_free(s_lyr);
		s_lyr = NULL;
	}
	s_lyr_n = 0;
}

void aster_audio_lyrics_load_path(const char *path, bool mp3)
{
	aster_audio_lyrics_clear();
	if (!path || !path[0]) {
		return;
	}
	if (mp3 && id3_from_file(path) > 0) {
		LOG_INF("lyrics id3 %uB", (unsigned)s_lyr_n);
		return;
	}
	if (sibling_lrc(path)) {
		LOG_INF("lyrics file %uB", (unsigned)s_lyr_n);
		return;
	}
	LOG_INF("lyrics none");
}

int aster_audio_lyrics_copy(char *dst, size_t max)
{
	size_t n;

	if (!dst || max == 0) {
		return -EINVAL;
	}
	n = s_lyr_n;
	if (n + 1 > max) {
		n = max - 1;
	}
	if (n && s_lyr) {
		memcpy(dst, s_lyr, n);
	}
	dst[n] = 0;
	return (int)n;
}
