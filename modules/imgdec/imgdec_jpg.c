/* SPDX-License-Identifier: MIT
 * JPEG decoder front-end over TJpgDec (vendored, R0.03 ChaN 2021).
 *
 * Streaming model, same philosophy as the BMP decoder: memory is decoupled
 * from BOTH the file size and the image size. We never materialise the whole
 * JPEG nor a full scaled raster:
 *   - the input stream is pulled straight from the SD file through infunc;
 *   - TJpgDec's free DCT scale (1/2,1/4,1/8) shrinks the source to `dec`,
 *     the coarsest factor that still covers the fit target;
 *   - outfunc receives MCU rectangles strictly top-to-bottom, left-to-right,
 *     which we accumulate into a two-row ping-pong buffer (dec_w uint16 each);
 *   - each time a source row is complete we emit the output rows whose
 *     vertical span it closes, bilinear/nearest sampling across the two
 *     cached rows -- identical weighting maths to the BMP scaler.
 *
 * Peak extra memory: one 12 KB TJpgDec pool + two dec_w rows + a JDEC struct,
 * independent of how large the JPEG or its raster is.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "imgdec.h"
#include "imgdec_internal.h"
#include "aster_bytes.h"
#include "aster_mem.h"
#include "aster_storage.h"
#include "vendor/tjpgd.h"

LOG_MODULE_REGISTER(imgdec_jpg, LOG_LEVEL_INF);

/* Refuse JPEGs whose compressed size exceeds this; a normal photo is 1-3 MB.
 * Only the size *reported by probe* uses this -- the streaming render never
 * buffers the whole file, so the cap is a sanity limit, not a memory limit. */
#define JPG_MAX_FILE (64u * 1024u * 1024u)

typedef struct {
	struct fs_file_t *f;      /* source stream (infunc) */

	/* scaled raster geometry */
	uint16_t *ring;           /* K rows x dec_w, slot = row % K */
	uint32_t dec_w, dec_h;    /* scaled source size */
	uint32_t K;               /* ring capacity (rows) >= 2*band_h + 2 */
	uint32_t cur_top;         /* scaled row where the current band starts */

	/* canvas (outfunc target) */
	uint16_t *out;
	uint32_t out_w, out_h;
	double s;                 /* out_w/dec_w == out_h/dec_h (<=1, downscale) */

	uint32_t next_out_y;      /* output rows are emitted in order */
	bool nearest;
} jpg_ctx_t;

/* Pull exactly nclb bytes of the JPEG from the SD file. TJpgDec treats any
 * short read as end-of-stream (JDR_INP), and FatFs may legitimately return
 * less than requested, so loop to fill. buff==NULL means "skip nclb bytes"
 * (APPn/EXIF segments during prepare) -- a forward seek is cheap on FatFs.
 * Returns bytes consumed (< nclb only at real EOF / I/O error). */
static size_t jpg_infunc(JDEC *jd, uint8_t *buff, size_t nclb)
{
	jpg_ctx_t *c = (jpg_ctx_t *)jd->device;
	if (nclb == 0) return 0;
	if (!buff) {
		return (fs_seek(c->f, (off_t)nclb, FS_SEEK_CUR) == 0) ? nclb : 0;
	}
	size_t done = 0;
	while (done < nclb) {
		int r = aster_fs_read_safe(c->f, buff + done, nclb - done);
		if (r <= 0) break;
		done += (size_t)r;
	}
	return done;
}

/* Emit every output row whose bottom span row is <= done_y (monotonic). */
static void jpg_emit(jpg_ctx_t *c, uint32_t done_y)
{
	const double s = c->s;
	while (c->next_out_y < c->out_h) {
		double cy = ((double)c->next_out_y + 0.5) / s - 0.5;
		int y0 = img_clampi((int)floor(cy), 0, (int)c->dec_h - 1);
		int y1 = c->nearest ? y0 : img_clampi(y0 + 1, 0, (int)c->dec_h - 1);
		/* can only finalise the row once both span rows exist */
		if ((uint32_t)y1 > done_y) break;

		const uint16_t *rA = c->ring + (size_t)(y0 % c->K) * c->dec_w;
		const uint16_t *rB = (y1 == y0) ? rA :
			c->ring + (size_t)(y1 % c->K) * c->dec_w;
		int iwy = (y1 == y0) ? 0 :
			img_clampi((int)((cy - (double)y0) * 256.0), 0, 256);
		uint16_t *orow = c->out + (size_t)c->next_out_y * c->out_w;

		for (uint32_t ox = 0; ox < c->out_w; ox++) {
			double cx = ((double)ox + 0.5) / s - 0.5;
			int x0 = img_clampi((int)floor(cx), 0, (int)c->dec_w - 1);
			int x1 = c->nearest ? x0 : img_clampi(x0 + 1, 0, (int)c->dec_w - 1);
			int iwx = (x1 == x0) ? 0 :
				img_clampi((int)((cx - (double)x0) * 256.0), 0, 256);
			uint32_t a = rA[x0], b = rA[x1], cc = rB[x0], dd = rB[x1];
			#define MIXC(v0,v1,i,mask) \
				((((v0)&(mask)) + ((((v1)&(mask)) - ((v0)&(mask))) * (i) >> 8)) & (mask))
			uint32_t tR = MIXC(a,b,iwx,0xF800), bR = MIXC(cc,dd,iwx,0xF800);
			uint32_t tG = MIXC(a,b,iwx,0x07E0), bG = MIXC(cc,dd,iwx,0x07E0);
			uint32_t tB = MIXC(a,b,iwx,0x001F), bB = MIXC(cc,dd,iwx,0x001F);
			#undef MIXC
			uint32_t R = (tR + (((bR - tR) * iwy) >> 8)) & 0xF800;
			uint32_t G = (tG + (((bG - tG) * iwy) >> 8)) & 0x07E0;
			uint32_t B = (tB + (((bB - tB) * iwy) >> 8)) & 0x001F;
			orow[ox] = (uint16_t)(R | G | B);
		}
		c->next_out_y++;
	}
}

static int jpg_outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
	jpg_ctx_t *c = (jpg_ctx_t *)jd->device;
	uint32_t rw = rect->right - rect->left + 1;
	uint32_t rh = rect->bottom - rect->top + 1;

	/* TJpgDec lays MCU rects down band by band: every rect in a band shares
	 * rect->top and, taken together across the row of MCUs, paints all
	 * `band_h` scaled rows of that band. A rect whose top is beyond the
	 * band we are filling means the previous band is now complete, so every
	 * source row up to (top-1) is fully written horizontally and its output
	 * rows may be safely emitted. Emitting earlier samples the not-yet-painted
	 * right-hand part of the band (the black-tail bug). */
	if ((uint32_t)rect->top > c->cur_top) {
		uint32_t done = (uint32_t)rect->top;      /* rows < done complete */
		if (done > c->dec_h) done = c->dec_h;
		jpg_emit(c, done - 1);
		c->cur_top = rect->top;
	}

	for (uint32_t y = 0; y < rh; y++) {
		uint32_t sy = (uint32_t)rect->top + y;
		uint16_t *dst = c->ring + (size_t)(sy % c->K) * c->dec_w + rect->left;
		const uint16_t *srow = (const uint16_t *)bitmap + (size_t)y * rw;
		/* TJpgDec's JD_FORMAT=1 packs the uint16 with B in bits[15:11]; the
		 * platform (and the BMP path) want R there. Exchange the two 5-bit
		 * fields once, here, so the ring is canonical RGB565. */
		for (uint32_t x = 0; x < rw; x++) {
			uint16_t v = srow[x];
			dst[x] = (uint16_t)(((v & 0x1F) << 11) | (v & 0x7E0) |
				      ((v >> 11) & 0x1F));
		}
	}
	return 1; /* keep decoding */
}

/* pick coarsest scale (0=1/1,1=1/2,2=1/4,3=1/8) whose output still >= target */
static uint8_t jpg_pick_scale(uint32_t sw, uint32_t sh, uint32_t tw, uint32_t th)
{
	for (uint8_t sc = 3;; sc--) {
		uint32_t f = 1u << sc;
		if (((sw + f - 1) / f >= tw && (sh + f - 1) / f >= th) || sc == 0)
			return sc;
	}
}

/* Parse SOFn for width/height without decoding. SOF payload (after the
 * 2-byte length field): [0]=precision [1..2]=height [3..4]=width [5]=ncomp.
 * Returns 0 + dims, or -1. */
static int jpg_parse_dims(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h,
			  uint32_t *ncomp)
{
	if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;
	size_t i = 2;
	while (i + 4 <= n) {
		if (d[i] != 0xFF) { i++; continue; }
		uint8_t m = d[i + 1];
		i += 2;
		if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7))
			continue;                        /* no length field */
		if (m == 0xD9 || i + 2 > n) break;       /* EOI / truncated */
		uint16_t seglen = aster_be16(d + i);
		if (seglen < 2 || i + seglen > n) break;
		const uint8_t *s = d + i + 2;            /* payload */
		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
			if (seglen < 7) return -1;
			*h = aster_be16(s + 1);
			*w = aster_be16(s + 3);
			*ncomp = s[5];
			return (*w && *h) ? 0 : -1;
		}
		i += seglen;
	}
	return -1;
}

aster_error_t aster_jpg_probe(const char *path, aster_imgdec_info_t *info,
			  uint32_t cap_w, uint32_t cap_h)
{
	struct fs_dirent de;
	if (fs_stat(path, &de) != 0 || de.type != FS_DIR_ENTRY_FILE)
		return ASTER_ENOENT;
	info->fmt = ASTER_IMG_JPG;
	info->file_size = (uint32_t)de.size;
	info->too_big = (uint32_t)de.size > JPG_MAX_FILE;
	if (info->too_big) {
		snprintf(info->why, sizeof(info->why), "JPG %u B > limit",
			 info->file_size);
		return ASTER_OK;
	}

	struct fs_file_t f;
	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) return ASTER_ENOENT;
	/* SOF usually sits in the first KB; a camera EXIF thumbnail can push
	 * it out, so read a 64 KB window into a scratch buffer (freed below). */
	size_t hn = de.size < 65536 ? (size_t)de.size : 65536;
	uint8_t *hd = aster_psram_alloc(hn);
	if (!hd) { fs_close(&f); return ASTER_ENOMEM; }
	int rd = aster_fs_read_safe(&f, hd, hn);
	fs_close(&f);
	if (rd < 4) { aster_psram_free(hd); return ASTER_ECORRUPT; }

	uint32_t w = 0, h = 0, nc = 0;
	int ok = jpg_parse_dims(hd, (size_t)rd, &w, &h, &nc);
	aster_psram_free(hd);
	if (ok != 0) {
		info->unsupported = true;
		snprintf(info->why, sizeof(info->why), "no SOF in header window");
		return ASTER_ECORRUPT;
	}
	info->src_w = w; info->src_h = h;
	double s;
	img_fit_size(w, h, cap_w, cap_h, &info->out_w, &info->out_h, &s);
	return ASTER_OK;
}

aster_error_t aster_jpg_render(const char *path, uint8_t *out,
			   uint32_t cap_w, uint32_t cap_h,
			   uint32_t *out_w, uint32_t *out_h, bool nearest)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) return ASTER_ENOENT;

	/* Fixed working pool: Huffman/DQT tables + MCU + IDCT + 512 B stream
	 * buffer. Measured need ~5-9 KB for baseline 4:2:0/4:4:4. */
	static const size_t POOL = 12 * 1024;
	uint8_t *pool = aster_psram_alloc(POOL);
	if (!pool) { fs_close(&f); return ASTER_ENOMEM; }

	jpg_ctx_t ctx = { .f = &f, .out = (uint16_t *)out, .nearest = nearest };
	JDEC jd;
	JRESULT jr = jd_prepare(&jd, jpg_infunc, pool, POOL, &ctx);
	if (jr != JDR_OK) {
		aster_psram_free(pool); fs_close(&f);
		LOG_WRN("jd_prepare=%d", (int)jr);
		return (jr == JDR_MEM1 || jr == JDR_MEM2) ? ASTER_ENOMEM : ASTER_ECORRUPT;
	}

	uint32_t sw = jd.width, sh = jd.height;   /* full source size */
	uint32_t tw, th; double s;
	img_fit_size(sw, sh, cap_w, cap_h, &tw, &th, &s);
	if (tw == 0 || th == 0) {
		aster_psram_free(pool); fs_close(&f); return ASTER_EINVAL;
	}

	uint8_t scale = jpg_pick_scale(sw, sh, tw, th);
	uint32_t f2 = 1u << scale;
	uint32_t dec_w = (sw + f2 - 1) / f2;
	uint32_t dec_h = (sh + f2 - 1) / f2;
	/* rows per MCU band, in scaled coords (TJpgDec shifts my>>scale) */
	uint32_t band_h = (jd.msy * 8) >> scale;
	if (band_h == 0) band_h = 1;

	/* Ring needs to hold the band being written plus the one whose output
	 * rows may still straddle it; 2*band_h + 2 is safe for any bilinear. */
	uint32_t K = 2 * band_h + 2;
	ctx.ring = aster_psram_alloc((size_t)K * dec_w * 2);
	if (!ctx.ring) {
		aster_psram_free(pool); fs_close(&f);
		LOG_WRN("jpg: ring %u B fail", (uint32_t)(K * dec_w * 2));
		return ASTER_ENOMEM;
	}

	ctx.dec_w = dec_w; ctx.dec_h = dec_h;
	ctx.K = K;
	ctx.out_w = tw; ctx.out_h = th;
	ctx.s = (double)tw / (double)dec_w;
	ctx.cur_top = 0; ctx.next_out_y = 0;

	jr = jd_decomp(&jd, jpg_outfunc, scale);
	jpg_emit(&ctx, dec_h - 1);   /* flush rows of the final band */

	aster_psram_free(ctx.ring);
	aster_psram_free(pool);
	fs_close(&f);
	if (jr != JDR_OK) {
		LOG_WRN("jd_decomp=%d", (int)jr);
		return (jr == JDR_MEM1 || jr == JDR_MEM2) ? ASTER_ENOMEM : ASTER_ECORRUPT;
	}
	if (ctx.next_out_y != th) {
		LOG_WRN("jpg: emitted %u/%u rows", ctx.next_out_y, th);
		return ASTER_ECORRUPT;
	}
	if (out_w) *out_w = tw;
	if (out_h) *out_h = th;
	return ASTER_OK;
}
