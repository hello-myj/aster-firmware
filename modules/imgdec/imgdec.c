/* SPDX-License-Identifier: MIT
 * Image decoder front-end + streaming BMP.
 *
 * BMP is the only truly uncompressed random-accessible bitmap: pixel rows
 * sit at arithmetic offsets, so a preview of a huge (4K/8K) BMP is produced
 * WITHOUT ever materialising the source -- we seek to the <=2 source rows
 * each output row needs and bilinear-sample across them. Memory is thus
 * decoupled from file size (constant: 2 row buffers + the RGB565 output).
 *
 * JPEG: TJpgDec streamed off the file with the coarsest DCT scale that
 * still covers the fit target; MCU bands -> K-row ring -> scaler (imgdec_jpg.c).
 * PNG: miniz tinfl streamed off the IDAT chunks; rows unfiltered on the fly
 * into a 2-row RGB565 cache -> scaler (imgdec_png.c). Neither path ever
 * materialises the full raster, so memory stays decoupled from image size.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include "imgdec.h"
#include "imgdec_internal.h"
#include "aster_bytes.h"
#include "aster_mem.h"
#include "aster_storage.h"
#include "aster_platform.h"

LOG_MODULE_REGISTER(imgdec, LOG_LEVEL_INF);

#define BMP_MAX_W   8192u   /* row-buffer ceiling: 32bpp row = 32 KiB */

/* fit: largest scale so src fits cap, aspect kept (never upscale). */
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

void img_fit_size(uint32_t sw, uint32_t sh, uint32_t cw, uint32_t ch,
		     uint32_t *ow, uint32_t *oh, double *scale)
{
	if (sw == 0 || sh == 0) { *ow = 0; *oh = 0; *scale = 1.0; return; }
	double s = (double)cw / (double)sw;
	double sy = (double)ch / (double)sh;
	if (sy < s) s = sy;
	if (s > 1.0) s = 1.0;      /* never upscale a preview */
	uint32_t w = (uint32_t)((double)sw * s + 0.5);
	uint32_t h = (uint32_t)((double)sh * s + 0.5);
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	*ow = w; *oh = h; *scale = s;
}

/* ---- BMP decode helpers ---- */

typedef struct {
	uint32_t w, h;         /* absolute */
	int32_t  hs;           /* raw signed height (top-down if <0) */
	uint32_t bpp;
	uint32_t compression;  /* 0=BI_RGB 3=BI_BITFIELDS */
	uint32_t off_bits;     /* pixel array offset */
	uint32_t stride;       /* bytes per source row (4-aligned) */
	const uint8_t *pal;    /* BGRA color table entries (may be NULL) */
	uint32_t pal_n;
} bmp_t;

/* Extract RGB888 of source column x from a fully-read row buffer. */
static inline void bmp_px(const bmp_t *b, const uint8_t *row, uint32_t x,
			  uint8_t *r, uint8_t *g, uint8_t *bl)
{
	uint32_t i = 0;
	switch (b->bpp) {
	case 24: { const uint8_t *p = row + (size_t)x * 3;
		   *bl = p[0]; *g = p[1]; *r = p[2]; return; }
	case 32: { const uint8_t *p = row + (size_t)x * 4;
		   *bl = p[0]; *g = p[1]; *r = p[2]; return; }
	case 8:  i = row[x]; break;
	case 4:  i = (x & 1) ? (row[x >> 1] & 0xF) : (row[x >> 1] >> 4); break;
	case 1:  i = (row[x >> 3] >> (7 - (x & 7))) & 1; break;
	default: *r = *g = *bl = 0; return;
	}
	if (b->pal && i < b->pal_n) {
		const uint8_t *p = b->pal + (size_t)i * 4;
		*bl = p[0]; *g = p[1]; *r = p[2];
	} else {
		*bl = *g = *r = i ? 255 : 0;
	}
}

static int bmp_read_header(struct fs_file_t *f, uint8_t fh[14], uint8_t dib[40])
{
	if (aster_fs_read_safe(f, fh, 14) != 14) return -EIO;
	if (fh[0] != 'B' || fh[1] != 'M') return -EINVAL;
	uint32_t dib_size = 0;
	if (aster_fs_read_safe(f, &dib[0], 4) != 4) return -EIO;   /* biSize */
	dib_size = aster_le32(dib);
	if (dib_size < 12) return -EINVAL;
	/* we always parse the first 40 bytes; short (12/16) headers padded 0 */
	memset(dib, 0, 40);
	uint32_t keep = dib_size < 40 ? dib_size : 40;
	if (fs_seek(f, 14, FS_SEEK_SET) != 0) return -EIO;
	if (aster_fs_read_safe(f, dib, keep) != (int)keep) return -EIO;
	return 0;
}

static aster_error_t bmp_probe_fill(bmp_t *b, const uint8_t dib[40],
				  uint32_t file_size, aster_imgdec_info_t *info)
{
	b->w = aster_le32(&dib[4]);
	b->hs = (int32_t)aster_le32(&dib[8]);
	b->h = (uint32_t)(b->hs < 0 ? -b->hs : b->hs);
	b->bpp = aster_le16(&dib[14]);
	b->compression = aster_le32(&dib[16]);
	/* NOTE: off_bits is set by the caller from the file header; do NOT
	 * touch it here (zeroing it made the renderer read each row 54 bytes
	 * early -> a whole-row horizontal shift). */

	bool ok_bpp = (b->bpp == 1 || b->bpp == 4 || b->bpp == 8 ||
		       b->bpp == 24 || b->bpp == 32);
	bool ok_comp = (b->compression == 0) ||
		       (b->compression == 3 && b->bpp == 32);

	if (info) {
		info->fmt = ASTER_IMG_BMP;
		info->src_w = b->w; info->src_h = b->h;
		info->file_size = file_size;
		info->unsupported = !ok_bpp || !ok_comp || b->w == 0 || b->h == 0 ||
				    b->w > BMP_MAX_W;
		if (!ok_comp)
			snprintf(info->why, sizeof(info->why),
				 "BMP compression %u / bpp %u", b->compression, b->bpp);
		else if (b->w > BMP_MAX_W)
			snprintf(info->why, sizeof(info->why),
				 "BMP width %u > %u", b->w, BMP_MAX_W);
		else if (!ok_bpp)
			snprintf(info->why, sizeof(info->why), "BMP bpp %u", b->bpp);
		info->too_big = false; /* streamed: size-independent memory */
	}
	if (!ok_bpp || !ok_comp || b->w == 0 || b->h == 0 || b->w > BMP_MAX_W)
		return ASTER_ENOTSUP;
	b->stride = ((b->bpp * b->w + 31u) / 32u) * 4u;
	return ASTER_OK;
}

/* aster_imgdec_render for a BMP: streaming bilinear/nearest row-sample. */
static aster_error_t bmp_render(const char *path, uint8_t *out,
			      uint32_t cap_w, uint32_t cap_h,
			      uint32_t *out_w, uint32_t *out_h, bool nearest)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	int err = fs_open(&f, path, FS_O_READ);
	if (err) { return ASTER_ENOENT; }

	uint8_t fh[14], dib[40];
	if (bmp_read_header(&f, fh, dib) != 0) { fs_close(&f); return ASTER_ECORRUPT; }

	bmp_t b; memset(&b, 0, sizeof(b));
	b.off_bits = aster_le32(&fh[10]);
	if (bmp_probe_fill(&b, dib, 0, NULL) != ASTER_OK) { fs_close(&f); return ASTER_ENOTSUP; }

	/* palette (bitCount<=8): sits right after the DIB header */
	uint8_t *pal_buf = NULL;
	b.pal = NULL;
	if (b.bpp <= 8) {
		pal_buf = aster_psram_alloc(256 * 4);
		uint32_t used = aster_le32(&dib[24]);
		if (used == 0) used = 1u << b.bpp;
		if (used > 256) used = 256;
		uint32_t dib_size = aster_le32(&dib[0]);
		uint32_t pal_file_off = 14 + dib_size;
		if (b.off_bits < pal_file_off + used * 4 && b.off_bits >= pal_file_off)
			used = (b.off_bits - pal_file_off) / 4;
		if (fs_seek(&f, pal_file_off, FS_SEEK_SET) == 0 &&
		    aster_fs_read_safe(&f, pal_buf, used * 4) == (int)(used * 4)) {
			b.pal = pal_buf; b.pal_n = used;
		}
	}

	uint32_t ow, oh; double s;
	img_fit_size(b.w, b.h, cap_w, cap_h, &ow, &oh, &s);
	if (ow == 0 || oh == 0) { fs_close(&f); return ASTER_EINVAL; }

	uint8_t *rowA = aster_psram_alloc(b.stride);
	uint8_t *rowB = aster_psram_alloc(b.stride);
	if (!rowA || !rowB) { aster_psram_free(rowA); aster_psram_free(rowB);
			       fs_close(&f); return ASTER_ENOMEM; }

	/* Slide through the source in FILE-OFFSET order so every fs_seek is a
	 * small forward hop. FatFs can only fast-forward a cluster chain; a
	 * backwards seek re-walks it from the start, which on a 72 MB / 6000-row
	 * bottom-up BMP costs ~175 ms per seek. Bottom-up rows are stored last
	 * row first, so we walk output rows in the direction that keeps the
	 * source file offset monotonically increasing, and fill the canvas by
	 * row index (random writes into `out` are fine). A two-row window keyed
	 * on file-row number tracks the source rows straddling each output row;
	 * each source row is read at most once -> constant memory, forward-only
	 * seeks, O(rows) SD traffic, never the full file. */
	int haveA = -1, haveB = -1;      /* file-row numbers resident */
	uint64_t max_loaded = 0;         /* highest file byte read so far */
	err = 0;

	/* file-row number of a visual row (top-down: identity; bottom-up: flip) */
	#define FR(v) ((uint32_t)((b.hs < 0) ? (v) : (int)(b.h - 1 - (v))))
	/* ensure visual row `v` is resident; returns its buffer (NULL on a
	 * would-be-backwards seek or read error). Loads strictly forward and
	 * evicts the older of the two cached rows. */
	#define ENSURE(v) ({ uint32_t _fv = FR(v); uint8_t *_p; \
		if (haveA == (int)_fv) _p = rowA; \
		else if (haveB == (int)_fv) _p = rowB; \
		else { uint64_t _off = (uint64_t)b.off_bits + (uint64_t)_fv * b.stride; \
			uint8_t **_slot = (haveA <= haveB) ? &rowA : &rowB; \
			int *_held = (haveA <= haveB) ? &haveA : &haveB; \
			if (_off < max_loaded) { _p = NULL; } \
			else if (fs_seek(&f, (off_t)_off, FS_SEEK_SET) == 0 && \
				 aster_fs_read_safe(&f, *_slot, b.stride) == (int)b.stride) { \
				*_held = (int)_fv; max_loaded = _off + b.stride; _p = *_slot; \
			} else { _p = NULL; } } \
		_p; })

	uint16_t *o16 = (uint16_t *)out;
	const int step = (b.hs < 0) ? 1 : -1;   /* keep file offset ascending */
	const uint32_t oy_start = (b.hs < 0) ? 0 : oh - 1;
	const uint32_t oy_end   = (b.hs < 0) ? oh : (uint32_t)-1; /* exclusive sentinel */

	for (uint32_t oy = oy_start; oy != oy_end; oy += step) {
		double cy = ((double)oy + 0.5) / s - 0.5;
		int y0 = clampi((int)floor(cy), 0, (int)b.h - 1);
		int y1 = clampi(y0 + 1, 0, (int)b.h - 1);
		double wy = clampd((y1 == y0) ? 0.0 : (cy - (double)y0), 0.0, 1.0);
		if (nearest) {
			/* snap to a single source row: read 1 row per output row */
			y0 = clampi((int)(cy + 0.5), 0, (int)b.h - 1);
			y1 = y0;
			wy = 0.0;
		}

		/* load in ascending file-row order to stay forward-only */
		uint8_t *rowHi;             /* buffer holding visual row y0 */
		const uint8_t *rb;          /* buffer holding y1 (== y0 in nearest) */
		{
			uint32_t f0 = FR(y0), f1 = FR(y1);
			uint8_t *p0, *p1;
			if (f0 <= f1) {
				p0 = ENSURE(y0);
				p1 = p0 ? ENSURE(y1) : NULL;
			} else {
				p1 = ENSURE(y1);
				p0 = p1 ? ENSURE(y0) : NULL;
			}
			if (!p0 || !p1) { err = -1; break; }
			rowHi = p0;
			rb = (y1 == y0) ? p0 : p1;
		}

		uint16_t *orow = o16 + (size_t)oy * ow;
		for (uint32_t ox = 0; ox < ow; ox++) {
			double cx = ((double)ox + 0.5) / s - 0.5;
			uint8_t r, g, bl;
			if (nearest) {
				int x = clampi((int)(cx + 0.5), 0, (int)b.w - 1);
				bmp_px(&b, rowHi, x, &r, &g, &bl); /* y snapped: rowHi only */
			} else {
				int x0 = clampi((int)floor(cx), 0, (int)b.w - 1);
				int x1 = clampi(x0 + 1, 0, (int)b.w - 1);
				double wx = clampd((x1 == x0) ? 0.0 : (cx - (double)x0), 0.0, 1.0);
				uint8_t r00,g00,b00,r01,g01,b01,r10,g10,b10,r11,g11,b11;
				bmp_px(&b, rowHi, x0, &r00,&g00,&b00);
				bmp_px(&b, rowHi, x1, &r01,&g01,&b01);
				bmp_px(&b, rb,    x0, &r10,&g10,&b10);
				bmp_px(&b, rb,    x1, &r11,&g11,&b11);
				int iwx = (int)(wx * 256), iwy = (int)(wy * 256);
				#define MIX2(a,bv,i) (uint8_t)((a) + (((int)(bv)-(int)(a)) * (i) >> 8))
				uint8_t ta = MIX2(r00,r01,iwx), tb = MIX2(r10,r11,iwx);
				r = MIX2(ta,tb,iwy);
				ta = MIX2(g00,g01,iwx); tb = MIX2(g10,g11,iwx);
				g = MIX2(ta,tb,iwy);
				ta = MIX2(b00,b01,iwx); tb = MIX2(b10,b11,iwx);
				bl = MIX2(ta,tb,iwy);
			}
			orow[ox] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
		}
	}
	aster_psram_free(rowA); aster_psram_free(rowB);
	aster_psram_free(pal_buf);
	fs_close(&f);
	if (err) return ASTER_EIO;
	if (out_w) *out_w = ow;
	if (out_h) *out_h = oh;
	return ASTER_OK;
}

/* ---- public API ---- */

const char *aster_imgdec_fmt_name(aster_imgdec_fmt_t fmt)
{
	switch (fmt) {
	case ASTER_IMG_BMP: return "BMP";
	case ASTER_IMG_JPG: return "JPG";
	case ASTER_IMG_PNG: return "PNG";
	default: return "?";
	}
}

aster_error_t aster_imgdec_probe(const char *path, aster_imgdec_info_t *info,
			  uint32_t cap_w, uint32_t cap_h)
{
	memset(info, 0, sizeof(*info));
	struct fs_dirent de;
	if (fs_stat(path, &de) != 0 || de.type != FS_DIR_ENTRY_FILE) {
		snprintf(info->why, sizeof(info->why), "not found");
		return ASTER_ENOENT;
	}
	info->file_size = (uint32_t)de.size;

	struct fs_file_t f; fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) {
		snprintf(info->why, sizeof(info->why), "open failed");
		return ASTER_ENOENT;
	}
	uint8_t sig[32];
	int n = aster_fs_read_safe(&f, sig, sizeof(sig));
	if (n < 2) { fs_close(&f); return ASTER_ECORRUPT; }

	aster_error_t rc = ASTER_OK;
	if (sig[0] == 'B' && sig[1] == 'M') {
		info->fmt = ASTER_IMG_BMP;
		uint8_t fh[14], dib[40];
		bmp_t b; memset(&b, 0, sizeof(b));
		if (fs_seek(&f, 0, FS_SEEK_SET) == 0 &&
		    bmp_read_header(&f, fh, dib) == 0) {
			b.off_bits = aster_le32(&fh[10]);
			uint32_t dib_size = aster_le32(&dib[0]);
			uint32_t used = aster_le32(&dib[24]);
			(void)dib_size; (void)used;
			rc = bmp_probe_fill(&b, dib, info->file_size, info);
			/* fill fit estimate regardless of support */
			if (b.w && b.h) { double s; img_fit_size(b.w, b.h, cap_w, cap_h,
							     &info->out_w, &info->out_h, &s); }
		} else {
			info->unsupported = true;
			snprintf(info->why, sizeof(info->why), "bad BMP header");
			rc = ASTER_ECORRUPT;
		}
	} else if (sig[0] == 0xFF && sig[1] == 0xD8) {
		fs_close(&f);
		return aster_jpg_probe(path, info, cap_w, cap_h);
	} else if (aster_le32(sig) == 0x474E5089u) {
		fs_close(&f);
		return aster_png_probe(path, info, cap_w, cap_h);
	} else {
		info->fmt = ASTER_IMG_UNKNOWN;
		info->unsupported = true;
		snprintf(info->why, sizeof(info->why), "unrecognised");
		rc = ASTER_EINVAL;
	}
	fs_close(&f);
	return rc;
}

aster_error_t aster_imgdec_render(const char *path, uint8_t *out,
			   uint32_t cap_w, uint32_t cap_h,
			   uint32_t *out_w, uint32_t *out_h, bool nearest)
{
	uint8_t sig[2];
	struct fs_file_t f; fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) return ASTER_ENOENT;
	int n = aster_fs_read_safe(&f, sig, 2);
	fs_close(&f);
	if (n < 2) return ASTER_ECORRUPT;
	if (sig[0] == 'B' && sig[1] == 'M')
		return bmp_render(path, out, cap_w, cap_h, out_w, out_h, nearest);
	if (sig[0] == 0xFF && sig[1] == 0xD8)
		return aster_jpg_render(path, out, cap_w, cap_h, out_w, out_h, nearest);
	if (sig[0] == 0x89 && sig[1] == 'P')
		return aster_png_render(path, out, cap_w, cap_h, out_w, out_h, nearest);
	return ASTER_ENOTSUP;
}

aster_error_t aster_imgdec_show(const char *path, bool nearest)
{
	aster_display_info_t d;
	if (aster_plat_display_get_info(0, &d) != ASTER_OK)
		return ASTER_ENODEV;

	uint32_t ow, oh;
	uint8_t *canvas = aster_psram_alloc((size_t)d.width * d.height * 2);
	if (!canvas) return ASTER_ENOMEM;

	aster_error_t rc = aster_imgdec_render(path, canvas, d.width, d.height,
					&ow, &oh, nearest);
	if (rc != ASTER_OK) { aster_psram_free(canvas); return rc; }

	aster_plat_display_clear(0x0000);
	uint32_t x = (d.width  > ow) ? (d.width  - ow) / 2 : 0;
	uint32_t y = (d.height > oh) ? (d.height - oh) / 2 : 0;
	rc = aster_plat_display_write_at(canvas, x, y, ow, oh, ow);
	/* DISP_ASYNC: write_at only queues the rect to the SPI worker; block
	 * until it is fully on the panel BEFORE freeing the PSRAM canvas, or
	 * the worker reads freed memory (use-after-free -> crash on big imgs). */
	aster_plat_display_flush_drain();
	aster_psram_free(canvas);
	return rc;
}
