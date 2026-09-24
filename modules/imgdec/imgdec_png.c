/* SPDX-License-Identifier: MIT
 * PNG decoder over miniz tinfl (vendored 3.0.2, public domain).
 *
 * Streaming model, same philosophy as the BMP/JPEG paths: memory is
 * decoupled from file size AND raster size. PNG rows arrive strictly
 * top-to-bottom and unfiltering only needs the previous raw row, so we
 * inflate straight into a fixed 32 KB wrapping dictionary, assemble one
 * row at a time, unfilter + convert it to RGB565 (alpha composited over
 * black), and emit every output row whose vertical sample span is now
 * closed -- identical bilinear maths to the JPEG scaler.
 *
 * Supported: non-interlaced, colour types 0/2/3/4/6, bit depths
 * 1/2/4/8/16 (16-bit keeps the MSB; sub-byte grays/palettes scale up).
 * Adam7 interlace is refused (rare for photos, needs 7 passes of state).
 *
 * Peak extra memory: 32 KB in + 32 KB dict + 2 raw rows + 2 RGB565 rows
 * in PSRAM (independent of image/file size), plus an ~8 KB tinfl state
 * machine pinned to internal DRAM (see aster_png_render for why).
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "imgdec.h"
#include "imgdec_internal.h"
#include "aster_bytes.h"
#include "aster_mem.h"
#include "aster_storage.h"
#include "vendor/miniz.h"

LOG_MODULE_REGISTER(imgdec_png, LOG_LEVEL_INF);

/* Row-buffer ceiling: 16384 px * 4 ch * 2 B = 128 KiB raw row (PSRAM). */
#define PNG_MAX_W 16384u

/* Concatenated view of all IDAT payloads: transparently skips chunk
 * framing (length/type at boundaries, CRC after each payload, non-IDAT
 * chunks seeked over) and stops at IEND. */
typedef struct {
	struct fs_file_t *f;
	uint32_t chunk_left;   /* bytes remaining in current IDAT */
	bool eof;              /* IEND seen, or any I/O failure */
} pngsrc_t;

/* Fill exactly want bytes into dst via read_safe, which may short-return at
 * cluster boundaries (its bounce path treats r<chunk as EOF). Retry from the
 * advanced file pos until full; stop only on genuine EOF (0) or error (<0).
 * Returns bytes filled (< want only at real EOF / I/O error). */
static size_t png_fill(struct fs_file_t *f, uint8_t *dst, size_t want)
{
	size_t got = 0;
	while (got < want) {
		int r = aster_fs_read_safe(f, dst + got, want - got);
		if (r <= 0) break;
		got += (size_t)r;
	}
	return got;
}

static size_t pngsrc_read(pngsrc_t *s, uint8_t *dst, size_t want)
{
	size_t got = 0;
	while (got < want && !s->eof) {
		if (s->chunk_left == 0) {
			uint8_t hdr[8];
			if (png_fill(s->f, hdr, 8) != 8) { s->eof = true; break; }
			uint32_t len = aster_be32(hdr);
			if (memcmp(&hdr[4], "IDAT", 4) == 0) {
				s->chunk_left = len;
			} else if (memcmp(&hdr[4], "IEND", 4) == 0) {
				s->eof = true; break;
			} else {
				if (fs_seek(s->f, (off_t)len + 4, FS_SEEK_CUR) != 0) {
					s->eof = true; break;
				}
			}
			continue;
		}
		size_t n = (want - got < s->chunk_left) ? want - got : s->chunk_left;
		int r = aster_fs_read_safe(s->f, dst + got, n);
		if (r <= 0) { s->eof = true; break; }
		got += (size_t)r;
		s->chunk_left -= (uint32_t)r;
		if (s->chunk_left == 0 && fs_seek(s->f, 4, FS_SEEK_CUR) != 0)
			s->eof = true;   /* CRC skip failed: treat as corrupt */
	}
	return got;
}

typedef struct {
	/* source geometry / format */
	uint32_t w, h;
	uint8_t bd, ct;            /* bit depth, colour type */
	uint8_t channels, bits;    /* per pixel */
	uint32_t row_bytes, row_total, bpp;
	uint8_t *pal;  uint32_t pal_n;    /* RGB triples */
	uint8_t *trns; uint32_t trns_n;   /* palette alphas */

	/* row assembly (two raw rows ping-pong; rowA accumulates) */
	uint8_t *rowA, *rowB;      /* 1 + row_bytes each; rowB = previous */
	uint32_t fill;
	uint32_t sy;               /* index of the row being filled */

	/* RGB565 row cache: prev565 = row sy-1, cur565 = row sy */
	uint16_t *cur565, *prev565;
	uint16_t *out;
	uint32_t out_w, out_h;
	double s;
	bool nearest;
	uint32_t next_out_y;
} pngc_t;

/* Extract sample #idx (bd bits, MSB-first packing) scaled to 0..255. */
static inline uint32_t png_sample(const uint8_t *d, uint32_t idx, uint8_t bd)
{
	if (bd == 8)  return d[idx];
	if (bd == 16) return d[idx * 2];            /* MSB wins */
	uint32_t bit = idx * bd;
	uint32_t v = (d[bit >> 3] >> (8 - bd - (bit & 7))) & ((1u << bd) - 1u);
	return (bd == 1) ? (v ? 255u : 0u) : (bd == 2) ? v * 85u : v * 17u;
}

static inline uint16_t png565(uint32_t r, uint32_t g, uint32_t b)
{
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Emit output rows whose bottom span row is <= sy. top/bot are the RGB565
 * caches of source rows sy / sy-1 (bot unused when nearest or y1 == y0).
 * Monotonic via next_out_y; same weighting maths as the JPEG path. */
static void png_emit(pngc_t *c, const uint16_t *top, const uint16_t *bot,
		     uint32_t sy)
{
	const double s = c->s;
	while (c->next_out_y < c->out_h) {
		double cy = ((double)c->next_out_y + 0.5) / s - 0.5;
		int y0 = img_clampi((int)floor(cy), 0, (int)c->h - 1);
		int y1 = c->nearest ? y0 : img_clampi(y0 + 1, 0, (int)c->h - 1);
		if ((uint32_t)y1 > sy) break;

		const uint16_t *rA = ((uint32_t)y0 == sy) ? top : bot;
		const uint16_t *rB = (y1 == y0) ? rA :
			(((uint32_t)y1 == sy) ? top : bot);
		int iwy = (y1 == y0) ? 0 :
			img_clampi((int)((cy - (double)y0) * 256.0), 0, 256);
		uint16_t *orow = c->out + (size_t)c->next_out_y * c->out_w;

		for (uint32_t ox = 0; ox < c->out_w; ox++) {
			double cx = ((double)ox + 0.5) / s - 0.5;
			int x0 = img_clampi((int)floor(cx), 0, (int)c->w - 1);
			int x1 = c->nearest ? x0 : img_clampi(x0 + 1, 0, (int)c->w - 1);
			int iwx = (x1 == x0) ? 0 :
				img_clampi((int)((cx - (double)x0) * 256.0), 0, 256);
			uint32_t a0 = rA[x0], b0 = rA[x1], c0 = rB[x0], d0 = rB[x1];
			#define MIXC(v0,v1,i,mask) \
				((((v0)&(mask)) + ((((v1)&(mask)) - ((v0)&(mask))) * (i) >> 8)) & (mask))
			uint32_t tR = MIXC(a0,b0,iwx,0xF800), bR = MIXC(c0,d0,iwx,0xF800);
			uint32_t tG = MIXC(a0,b0,iwx,0x07E0), bG = MIXC(c0,d0,iwx,0x07E0);
			uint32_t tB = MIXC(a0,b0,iwx,0x001F), bB = MIXC(c0,d0,iwx,0x001F);
			#undef MIXC
			uint32_t R = (tR + (((bR - tR) * iwy) >> 8)) & 0xF800;
			uint32_t G = (tG + (((bG - tG) * iwy) >> 8)) & 0x07E0;
			uint32_t B = (tB + (((bB - tB) * iwy) >> 8)) & 0x001F;
			orow[ox] = (uint16_t)(R | G | B);
		}
		c->next_out_y++;
	}
}

/* Unfilter rowA in place (rowB holds the previous raw row), convert to
 * cur565 (alpha composited over black), then close output rows. */
static int png_row(pngc_t *c)
{
	const uint8_t f = c->rowA[0];
	uint8_t *d = c->rowA + 1;
	uint8_t *pr = c->rowB + 1;
	uint32_t n = c->row_bytes, bpp = c->bpp;

	switch (f) {
	case 0: break;
	case 1: for (uint32_t i = bpp; i < n; i++) d[i] += d[i - bpp]; break;
	case 2: for (uint32_t i = 0; i < n; i++) d[i] += pr[i]; break;
	case 3: for (uint32_t i = 0; i < n; i++) {
			uint32_t a = (i >= bpp) ? d[i - bpp] : 0;
			d[i] += (uint8_t)((a + pr[i]) >> 1);
		} break;
	case 4: for (uint32_t i = 0; i < n; i++) {
			int a = (i >= bpp) ? d[i - bpp] : 0;
			int bb = pr[i];
			int cc = (i >= bpp) ? pr[i - bpp] : 0;
			int p = a + bb - cc, pa = abs(p - a), pb = abs(p - bb),
			    pc = abs(p - cc);
			d[i] += (uint8_t)((pa <= pb && pa <= pc) ? a :
			                  (pb <= pc) ? bb : cc);
		} break;
	default:
		LOG_WRN("png: bad filter %u at row %u", f, c->sy);
		return -1;
	}

	/* convert to RGB565 */
	for (uint32_t x = 0; x < c->w; x++) {
		uint32_t r, g, b, a = 255;
		switch (c->ct) {
		case 0: r = g = b = png_sample(d, x, c->bd); break;
		case 2: r = png_sample(d, 3*x, c->bd);
		        g = png_sample(d, 3*x+1, c->bd);
		        b = png_sample(d, 3*x+2, c->bd); break;
		case 3: {
			uint32_t idx = (c->bd == 8) ? d[x]
				: png_sample(d, x, c->bd) >> (8 - c->bd);
			if (idx >= c->pal_n) { r = g = b = 0; break; }
			r = c->pal[idx*3]; g = c->pal[idx*3+1]; b = c->pal[idx*3+2];
			if (c->trns && idx < c->trns_n) a = c->trns[idx];
			break;
		}
		case 4: r = g = b = png_sample(d, 2*x, c->bd);
		        a = png_sample(d, 2*x+1, c->bd); break;
		default: /* 6 */
			r = png_sample(d, 4*x, c->bd);
			g = png_sample(d, 4*x+1, c->bd);
			b = png_sample(d, 4*x+2, c->bd);
			a = png_sample(d, 4*x+3, c->bd); break;
		}
		c->cur565[x] = png565((r * a) / 255u, (g * a) / 255u, (b * a) / 255u);
	}

	/* cache pair holds rows sy (cur) and sy-1 (prev): close every
	 * output row whose span bottom is <= sy */
	png_emit(c, c->cur565, c->prev565, c->sy);

	/* roll rows: A becomes the new "previous", B the new scratch */
	uint8_t *t = c->rowB; c->rowB = c->rowA; c->rowA = t;
	uint16_t *t5 = c->prev565; c->prev565 = c->cur565; c->cur565 = t5;
	c->fill = 0;
	c->sy++;
	return 0;
}

/* returns 0 = fine, 1 = whole raster consumed, -1 = corrupt */
static int png_feed(pngc_t *c, const uint8_t *p, size_t n)
{
	while (n--) {
		if (c->sy >= c->h) return 1;   /* trailing junk after raster */
		c->rowA[c->fill++] = *p++;
		if (c->fill == c->row_total) {
			if (png_row(c) != 0) return -1;
		}
	}
	return (c->sy >= c->h) ? 1 : 0;
}

/* variant support gate shared by probe + render */
static bool png_variant_ok(uint8_t bd, uint8_t ct, uint8_t interlace)
{
	if (interlace != 0) return false;
	switch (ct) {
	case 0: return bd == 1 || bd == 2 || bd == 4 || bd == 8 || bd == 16;
	case 2: return bd == 8 || bd == 16;
	case 3: return bd == 1 || bd == 2 || bd == 4 || bd == 8;
	case 4: return bd == 8 || bd == 16;
	case 6: return bd == 8 || bd == 16;
	default: return false;
	}
}

static uint8_t png_channels(uint8_t ct)
{
	/* indexed by colour type directly: 0=G 2=RGB 3=palette 4=GA 6=RGBA */
	static const uint8_t ch[7] = { 1, 0, 3, 1, 2, 0, 4 };
	return (ct < 7) ? ch[ct] : 0;
}

aster_error_t aster_png_probe(const char *path, aster_imgdec_info_t *info,
			  uint32_t cap_w, uint32_t cap_h)
{
	struct fs_dirent de;
	if (fs_stat(path, &de) != 0 || de.type != FS_DIR_ENTRY_FILE)
		return ASTER_ENOENT;
	info->fmt = ASTER_IMG_PNG;
	info->file_size = (uint32_t)de.size;

	struct fs_file_t f; fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) return ASTER_ENOENT;
	uint8_t ih[29];   /* sig(8) + len(4) + "IHDR"(4) + data(13) */
	size_t n = png_fill(&f, ih, sizeof(ih));
	fs_close(&f);
	if (n < sizeof(ih) || memcmp(&ih[12], "IHDR", 4) != 0) {
		info->unsupported = true;
		snprintf(info->why, sizeof(info->why), "bad PNG header");
		return ASTER_ECORRUPT;
	}
	uint32_t w = aster_be32(&ih[16]), h = aster_be32(&ih[20]);
	uint8_t bd = ih[24], ct = ih[25], il = ih[28];
	info->src_w = w; info->src_h = h;
	double s; img_fit_size(w, h, cap_w, cap_h, &info->out_w, &info->out_h, &s);

	if (w == 0 || h == 0) {
		info->unsupported = true;
		snprintf(info->why, sizeof(info->why), "png 0x0");
	} else if (w > PNG_MAX_W) {
		info->too_big = true;
		snprintf(info->why, sizeof(info->why), "PNG w=%u > %u", w, PNG_MAX_W);
	} else if (!png_variant_ok(bd, ct, il)) {
		info->unsupported = true;
		if (il) snprintf(info->why, sizeof(info->why), "interlaced (adam7)");
		else snprintf(info->why, sizeof(info->why), "ct=%u bd=%u", ct, bd);
	}
	return ASTER_OK;
}

aster_error_t aster_png_render(const char *path, uint8_t *out,
			   uint32_t cap_w, uint32_t cap_h,
			   uint32_t *out_w, uint32_t *out_h, bool nearest)
{
	aster_error_t err = ASTER_ECORRUPT;
	struct fs_file_t f; fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) return ASTER_ENOENT;

	pngc_t c = { 0 };
	c.out = (uint16_t *)out;
	c.nearest = nearest;
	uint8_t *inb = NULL, *dict = NULL;
	tinfl_decompressor *tin = NULL;
	/* The tinfl state machine (bit-buffer + huffman tables) is read/written
	 * at bit granularity while the SD path shuttles bytes through an
	 * internal-RAM bounce buffer. On the ESP32-S3, PSRAM and SPI flash share
	 * the octal SPI bus, and keeping this hot state in PSRAM intermittently
	 * corrupts it (observed as a wrong inflate byte + heap-canary trip).
	 * Pin it to internal DRAM; only ~8 KB, and it is not reentrant. */
	static uint8_t dram_tin[sizeof(tinfl_decompressor)] __aligned(16);

	/* signature */
	uint8_t sig[8];
	if (png_fill(&f, sig, 8) != 8 ||
	    memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) goto out_close;

	/* scan chunks until IHDR (any leading ancillary chunk is skipped) */
	for (;;) {
		uint8_t ch[8];
		if (png_fill(&f, ch, 8) != 8) goto out_close;
		uint32_t len = aster_be32(ch);
		if (memcmp(&ch[4], "IHDR", 4) == 0 && len == 13) {
			uint8_t ih[13];
			if (png_fill(&f, ih, 13) != 13) goto out_close;
			c.w = aster_be32(&ih[0]); c.h = aster_be32(&ih[4]);
			c.bd = ih[8]; c.ct = ih[9];
			if (fs_seek(&f, 4, FS_SEEK_CUR) != 0) goto out_close;
			if (!png_variant_ok(c.bd, c.ct, ih[12]) || c.w == 0 ||
			    c.w > PNG_MAX_W || c.h == 0) goto out_close;
			break;
		}
		if (fs_seek(&f, (off_t)len + 4, FS_SEEK_CUR) != 0) goto out_close;
	}

	c.channels = png_channels(c.ct);
	c.bits = (uint8_t)(c.channels * c.bd);
	c.row_bytes = (uint32_t)(((uint64_t)c.w * c.bits + 7) / 8);
	c.row_total = c.row_bytes + 1;
	c.bpp = (c.bits >= 8) ? (uint32_t)(c.bits / 8u) : 1u;
	LOG_INF("png %ux%u bd=%u ct=%u ch=%u row_bytes=%u bpp=%u",
		c.w, c.h, c.bd, c.ct, c.channels, c.row_bytes, c.bpp);

	uint32_t tw, th; double s;
	img_fit_size(c.w, c.h, cap_w, cap_h, &tw, &th, &s);
	if (tw == 0 || th == 0) { err = ASTER_EINVAL; goto out_close; }
	c.out_w = tw; c.out_h = th; c.s = s;

	/* PSRAM working set */
	inb   = aster_psram_alloc(32768);
	dict  = aster_psram_alloc(TINFL_LZ_DICT_SIZE);
	tin   = (tinfl_decompressor *)dram_tin;   /* MUST stay in internal DRAM */
	c.rowA  = aster_psram_alloc(c.row_total);
	c.rowB  = aster_psram_alloc(c.row_total);
	c.cur565  = aster_psram_alloc((size_t)c.w * 2);
	c.prev565 = aster_psram_alloc((size_t)c.w * 2);
	c.pal   = aster_psram_alloc(768);
	c.trns  = aster_psram_alloc(256);
	if (!inb || !dict || !tin || !c.rowA || !c.rowB || !c.cur565 ||
	    !c.prev565 || !c.pal || !c.trns) { err = ASTER_ENOMEM; goto free_all; }
	memset(c.rowB, 0, c.row_total);      /* virtual row above the image */

	/* PLTE/tRNS must precede the first IDAT; stop the scan there */
	pngsrc_t src = { .f = &f };
	while (!src.eof && src.chunk_left == 0) {
		uint8_t ch[8];
		if (png_fill(&f, ch, 8) != 8) { src.eof = true; break; }
		uint32_t len = aster_be32(ch);
		if (memcmp(&ch[4], "PLTE", 4) == 0 && len <= 768) {
			if (png_fill(&f, c.pal, len) != len)
				goto free_all;
			c.pal_n = len / 3u;
			if (fs_seek(&f, 4, FS_SEEK_CUR) != 0) goto free_all;
		} else if (memcmp(&ch[4], "tRNS", 4) == 0 && len <= 256 &&
		           c.ct == 3) {
			if (png_fill(&f, c.trns, len) != len)
				goto free_all;
			c.trns_n = len;
			if (fs_seek(&f, 4, FS_SEEK_CUR) != 0) goto free_all;
		} else if (memcmp(&ch[4], "IDAT", 4) == 0) {
			src.chunk_left = len;    /* leave payload for the pump */
		} else if (memcmp(&ch[4], "IEND", 4) == 0) {
			src.eof = true;
		} else if (fs_seek(&f, (off_t)len + 4, FS_SEEK_CUR) != 0) {
			goto free_all;
		}
	}
	if (c.ct == 3 && c.pal_n == 0) goto free_all;   /* paletteless ct3 */

	tinfl_init(tin);
	uint32_t flags = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT;
	size_t avail = 0, pos = 0;
	/* Wrapping dictionary: match back-refs are resolved as
	 * (write_pos - dist) & 32767, so the write cursor must advance
	 * monotonically through the ring (as in tinfl_decompress_mem_to_
	 * callback); restarting every call at dict[0] would shred history. */
	size_t dict_ofs = 0;
	tinfl_status st = TINFL_STATUS_NEEDS_MORE_INPUT;
	for (;;) {
		if (pos == avail) {
			pos = avail = 0;
			avail = pngsrc_read(&src, inb, 32768);
			if (avail == 0) flags &= ~TINFL_FLAG_HAS_MORE_INPUT;
		}
		size_t in_used = avail - pos, out_used = TINFL_LZ_DICT_SIZE - dict_ofs;
		st = tinfl_decompress(tin, inb + pos, &in_used,
		                      dict, dict + dict_ofs, &out_used, flags);
		pos += in_used;
		if (out_used) {
			/* the fresh bytes sit at [dict_ofs, dict_ofs+out_used)
			 * and may wrap the ring: drain as up to two segments */
			size_t first = TINFL_LZ_DICT_SIZE - dict_ofs;
			if (first > out_used) first = out_used;
			int fr = png_feed(&c, dict + dict_ofs, first);
			if (fr == 0 && out_used > first)
				fr = png_feed(&c, dict, out_used - first);
			dict_ofs = (dict_ofs + out_used) & (TINFL_LZ_DICT_SIZE - 1);
			if (fr < 0) goto free_all;
			if (fr == 1) break;   /* raster complete */
		}
		if (st == TINFL_STATUS_DONE) break;
		if ((int)st < 0) goto free_all;
	}

	err = (c.next_out_y == th) ? ASTER_OK : ASTER_ECORRUPT;
	if (err == ASTER_OK) {
		if (out_w) *out_w = tw;
		if (out_h) *out_h = th;
	} else {
		LOG_WRN("png: emitted %u/%u rows (tinfl=%d)",
			c.next_out_y, th, (int)st);
	}

free_all:
	aster_psram_free(inb); aster_psram_free(dict);
	aster_psram_free(c.rowA); aster_psram_free(c.rowB);
	aster_psram_free(c.cur565); aster_psram_free(c.prev565);
	aster_psram_free(c.pal); aster_psram_free(c.trns);
out_close:
	fs_close(&f);
	return err;
}
