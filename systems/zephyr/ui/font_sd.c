/* SPDX-License-Identifier: MIT
 * cpfont v4 loader + lv_font_t backend.
 *
 * Mechanism ported from CrossPoint Reader CJK (MIT,
 * https://github.com/CrossPoint-CJK/crosspoint-reader-cjk); this is an
 * independent implementation of the same on-disk format and of its
 * three-tier idea: resident metrics (layout never touches SD), a
 * bump+epoch bitmap ring, and async burst fills behind a correct-sized
 * blank box that repaints when the real glyph lands. Format spec:
 * docs/cpfont-notes.md.
 *
 * Per baked size: EMPTY --(UI poll: header+TOC+intervals, one bounded
 * read)--> TABLE --(worker: compact metric table in 32 KB record
 * chunks)--> READY (UI poll flips state; only the UI thread writes
 * state). The compact metric table (8 B/glyph: adv 12.4, w, h, left,
 * top) ends fully resident for every size (~0.93 MB across 8 sizes on
 * PSRAM), so get_dsc is pure RAM. The on-disk 16 B record (adds dlen +
 * doff) is read only when a bitmap is fetched.
 *
 * Bitmap ring: one 48 KB bump+epoch arena per size (no per-object
 * malloc - CrossPoint's anti-fragmentation rule). warm() and render
 * misses enqueue (px, cp); the system workqueue pops a batch, sorts by
 * glyph index, merges contiguous runs, and per run does one open +
 * burst-read of the record span + burst-read of the covering bitmap
 * span (records and bitmaps are both glyph-index-ordered on disk).
 * Results land in a PSRAM staging pool; UI commit() moves them into the
 * ring and asks the caller for one repaint. A double-fetch (worker
 * races a warm hit) is benign: commit dedups against the ring.
 *
 * Locking discipline: s_lock covers queue + staging reservations only -
 * NEVER SD I/O - so no render-path call can be stalled by a slow card.
 * Worker lock passes are memcpy-short. Teardown (drop_all) resets state
 * under the lock, then k_work_cancel_sync() before freeing any table.
 */
#include "font_sd.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "aster_mem.h"
#include "aster_storage.h"

LOG_MODULE_REGISTER(aster_fontsd, LOG_LEVEL_INF);

#define CP_DIR          ASTER_MOUNT_POINT "/aster/fonts"
#define CPF_VERSION     4u
#define CPF_REC         16u		/* on-disk EpdGlyph size          */
/* Format v4 flags bit0 selects bitmap depth: 1 = 2bpp (CrossPoint default),
 * 0 = 4bpp (what ASTER bakes -- 16 levels, storage + ring bytes only; the
 * resident metric tables and A8 render scratch are depth-independent). */
#define METRIC_RECS     2048u		/* records streamed per pass      */
#define RING_BYTES      (48u * 1024u)	/* bitmap arena per size slot     */
#define RING_SLOTS      512u
#define STG_SLOTS       40u		/* workq -> UI staging entries    */
#define BURST_GLYPHS    48u		/* max glyphs merged per run      */
#define BATCH_MAX       64u		/* queue entries popped per pass  */
#define BURST_BYTES     (64u * 1024u)	/* worker read temp               */
#define MISS_Q          256u		/* pending codepoint fetches      */
#define PROBE_MS        2000u

/* These two are a property of the BAKE, not a policy choice: a glyph bigger
 * than either buffer is dropped, and until now it was dropped in silence.
 * Hand-picked values drifted from the bake twice. Both come from the bake
 * record instead (gen_faces.py -> ASTER_FONT_MAX_*), so re-baking re-sizes
 * them, and the asserts below pin them against the buffers they must fit in.
 * The 0 case is the "manifest predates these fields" fallback -- fail the
 * build rather than ship a loader that silently drops the largest glyphs. */
#define STG_BMP         (ASTER_FONT_MAX_BMP)
#define SCRATCH_BYTES   (ASTER_FONT_MAX_SCRATCH)

BUILD_ASSERT(ASTER_FONT_MAX_BMP > 0u && ASTER_FONT_MAX_SCRATCH > 0u,
	     "no bake limits in font_sd.h: run sdk/tools/gen_faces.py --write");
BUILD_ASSERT((uint64_t)ASTER_FONT_MAX_BMP <= (uint64_t)BURST_BYTES,
	     "a baked glyph is larger than the worker's read buffer");
BUILD_ASSERT((uint64_t)ASTER_FONT_MAX_BMP <= (uint64_t)RING_BYTES,
	     "a baked glyph cannot fit the bitmap arena");
/* SCRATCH_BYTES is w*h from the bake; that is the A8 need only while rows
 * are one byte per pixel. Assert the config, since a future stride change
 * would silently under-size the scratch and blank the widest glyphs. */
BUILD_ASSERT(LV_DRAW_BUF_STRIDE_ALIGN == 1,
	     "SCRATCH_BYTES is derived as w*h; A8 stride must equal width");

enum { ST_EMPTY = 0, ST_TABLE, ST_READY, ST_DEAD, ST_SPARSE };

/* A launcher caption plus dynamic titles can exceed 48 glyphs. Retain a
 * small working set across adjacent cards instead of refetching every
 * caption from SD. Offsets/counts below remain 16/8 bits respectively. */
#define SPARSE_BYTES (48u * 1024u)
#define SPARSE_N     192u
BUILD_ASSERT(SPARSE_BYTES <= UINT16_MAX && SPARSE_N <= UINT8_MAX,
             "sparse glyph cache exceeds its offset/count fields");

typedef struct {
	uint16_t adv;		/* 12.4 fixed-point */
	int8_t left;
	int8_t top;		/* baseline-up positive, as baked */
	uint8_t w;
	uint8_t h;
	uint16_t pad;
} cp_mt_t;
BUILD_ASSERT(sizeof(cp_mt_t) == 8);

typedef struct {		/* on-disk 16 B record */
	uint8_t w, h;
	uint16_t adv;
	int16_t left, top;
	uint16_t dlen;
	uint32_t doff;
} cp_rec_t;

typedef struct {
	uint32_t first, last, offset;
} cp_iv_t;

typedef struct {
	uint32_t cp;		/* 0 = free */
	uint32_t off;		/* arena byte offset */
	uint8_t epoch;
} ring_ent_t;

typedef struct {
	uint8_t px;		/* 0 = free */
	uint32_t cp;
} miss_ent_t;

typedef struct {
	uint8_t used;
	uint8_t px;
	uint32_t cp;
	uint16_t len;
	uint8_t bmp[STG_BMP];
} stg_t;

typedef struct {
	uint8_t px;		/* registered size, 0 = unused */
	volatile uint8_t state;	/* written by UI thread only */
	char path[64];
	uint32_t fsize;
	uint32_t iv_cnt, gl_cnt;
	uint32_t rec_foff, bm_foff;
	uint8_t advY;
	int16_t asc, desc;
	cp_iv_t *ivs;		/* UI writes at load, then read-only   */
	cp_mt_t *mts;		/* worker writes until READY, then RO  */
	volatile uint32_t mt_have;
	uint8_t *arena;		/* ring store: commit()/render (UI)   */
	uint8_t *scratch;	/* packed->A8 expansion: render (UI)  */
	uint8_t two_bpp;	/* file flags bit0: 1 = 2bpp, 0 = 4bpp  */
	lv_draw_buf_t dbuf;	/* returned by get_bitmap             */
	uint32_t bump;
	uint8_t epoch;
	ring_ent_t ring[RING_SLOTS];
	lv_font_t font;
} slot_t;

/* Packed levels -> A8. 4bpp LUT is level*17 (0..255 in 16 steps); 2bpp
 * keeps CrossPoint's levels. ASTER bakes 4bpp, so the hot path is the
 * (b[i>>1] >> 4/0) & 15 nibble select. */
static const uint8_t LUT2[4] = { 0, 85, 170, 255 };
static const uint8_t LUT4[16] = {
	0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255
};

static inline uint8_t pix2a(const uint8_t *b, uint32_t i, bool two_bpp)
{
	if (two_bpp) {
		return LUT2[(b[i >> 2] >> (6 - ((i & 3) << 1))) & 3];
	}
	return LUT4[(i & 1u) ? (b[i >> 1] & 0xFu) : (b[i >> 1] >> 4)];
}

/* Baked size table: one cpfont file (ui_<px>.cpfont) per entry. Generated
 * from the bake record (sdk/assets/fonts/faces.json) into font_sd.h, and
 * the only copy in the firmware -- ui_font.c iterates it through
 * aster_font_sd_face_px() rather than keeping a list of its own. It is also
 * what sizes the slot table below, so a new size grows s_slots with it
 * instead of running off its end. */
static const uint8_t s_baked_px[] = ASTER_FONT_BAKED_PX;

uint32_t aster_font_sd_face_n(void)
{
	return ARRAY_SIZE(s_baked_px);
}

uint8_t aster_font_sd_face_px(uint32_t i)
{
	return i < ARRAY_SIZE(s_baked_px) ? s_baked_px[i] : 0u;
}

/* Slot tables live in PSRAM (ring index arrays are ~1.2 KB/slot): keeps
 * internal DRAM at the pre-cpfont level. Accessed only from the UI thread
 * and the (rare) worker passes. */
#define CPF_SLOTS ARRAY_SIZE(s_baked_px)
static slot_t s_slots[CPF_SLOTS] __attribute__((section(".ext_ram.bss.font_sd")));
/* init() fills slot i from s_baked_px[i] for i < ARRAY_SIZE(s_baked_px), so
 * a slot table smaller than the size table would walk (and write) past its
 * end: silent corruption, no diagnostic. The count is derived above; this
 * assert keeps a future edit back to a literal count honest. */
BUILD_ASSERT(ARRAY_SIZE(s_slots) >= ARRAY_SIZE(s_baked_px),
	     "cpfont slot table smaller than the baked size table");
static stg_t *s_stg;
static miss_ent_t s_miss[MISS_Q] __attribute__((section(".ext_ram.bss.font_sd")));
static uint32_t s_miss_h, s_miss_t;
static uint8_t *s_burst;	/* worker-private read temp */
static bool s_want_full;
static uint32_t s_full_refs;
static uint8_t *s_sparse_mem;
static uint16_t s_sparse_used;
static uint8_t s_sparse_n;
static uint8_t s_sparse_scratch[SCRATCH_BYTES];
struct sparse_ent {
	uint8_t px;
	uint32_t cp;
	cp_rec_t rec;
	uint16_t off;
	uint16_t len;
};
static struct sparse_ent s_sparse[SPARSE_N] __attribute__((section(".ext_ram.bss.font_sd")));
static struct k_mutex s_lock;
static struct k_work_delayable s_work;
static atomic_t s_fetch_busy;
static uint32_t s_probe_ms;
static bool s_inited;
static bool s_probe_first;	/* first discovery sweeps all slots at once */
static uint32_t s_io_fails;	/* consecutive SD open/read failures */
static bool s_repaint;		/* latched "the picture changed" (see below) */
static uint32_t s_generation;	/* updates observed independently by native UI */
static bool s_oversize_warned;	/* announced once per boot                 */
static aster_font_sd_stats_t s_st;

/* A glyph the loader cannot stage is dropped from the frame with no other
 * symptom: the UI draws a hole where the character should be, which reads as
 * a font bug rather than as a buffer bug. That is exactly how the two
 * hand-picked buffer sizes below drifted from the bake unnoticed. Count every
 * one, and announce the first with enough detail to name the culprit (the
 * sizes themselves are in ASTER_FONT_MAX_* / STG_BMP / SCRATCH_BYTES). */
static void note_oversize(uint32_t px, uint32_t need, uint32_t cap)
{
	s_st.oversize++;
	if (!s_oversize_warned) {
		s_oversize_warned = true;
		LOG_WRN("cpfont %upx: glyph needs %u B, buffer is %u B -- "
			"dropping oversized glyphs (re-bake or re-push the "
			"card); further ones counted, not logged",
			(unsigned)px, (unsigned)need, (unsigned)cap);
	}
}

/* ------------------------------------------------------------------ *
 * small helpers
 * ------------------------------------------------------------------ */

static uint32_t ld32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t ld16s(const uint8_t *p)
{
	return (int16_t)(p[0] | (p[1] << 8));
}

static void rd_rec(cp_rec_t *r, const uint8_t *p)
{
	r->w = p[0];
	r->h = p[1];
	r->adv = (uint16_t)(p[2] | (p[3] << 8));
	r->left = ld16s(p + 4);
	r->top = ld16s(p + 6);
	r->dlen = (uint16_t)(p[8] | (p[9] << 8));
	r->doff = ld32(p + 12);
}

static uint32_t utf8_next(const uint8_t **p)
{
	const uint8_t *s = *p;
	uint8_t c = s[0];
	uint32_t cp, extra, i;

	if (c == 0) {
		return 0;
	}
	if (c < 0x80u) {
		*p = s + 1;
		return c;
	}
	if ((c & 0xE0u) == 0xC0u) {
		cp = c & 0x1Fu;
		extra = 1;
	} else if ((c & 0xF0u) == 0xE0u) {
		cp = c & 0x0Fu;
		extra = 2;
	} else if ((c & 0xF8u) == 0xF0u) {
		cp = c & 0x07u;
		extra = 3;
	} else {
		*p = s + 1;
		return 0xFFFD;
	}
	for (i = 0; i < extra; i++) {
		if ((s[1 + i] & 0xC0u) != 0x80u) {
			*p = s + 1;
			return 0xFFFD;
		}
		cp = (cp << 6) | (s[1 + i] & 0x3Fu);
	}
	*p = s + 1 + extra;
	return cp;
}

static slot_t *slot_by_px(uint8_t px)
{
	for (uint32_t i = 0; i < ARRAY_SIZE(s_slots); i++) {
		if (s_slots[i].px == px) {
			return &s_slots[i];
		}
	}
	return NULL;
}

static int32_t iv_find(const slot_t *s, uint32_t cp)
{
	uint32_t lo = 0, hi = s->iv_cnt;

	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2u;
		const cp_iv_t *iv = &s->ivs[mid];

		if (cp < iv->first) {
			hi = mid;
		} else if (cp > iv->last) {
			lo = mid + 1;
		} else {
			return (int32_t)(iv->offset + (cp - iv->first));
		}
	}
	return -1;
}

/* glyph index -> codepoint (binary search over intervals) */
static uint32_t iv_cp(const slot_t *s, uint32_t gidx)
{
	uint32_t lo = 0, hi = s->iv_cnt;

	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2u;
		const cp_iv_t *iv = &s->ivs[mid];
		uint32_t n = iv->last - iv->first + 1u;

		if (gidx < iv->offset) {
			hi = mid;
		} else if (gidx >= iv->offset + n) {
			lo = mid + 1;
		} else {
			return iv->first + (gidx - iv->offset);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * bitmap ring (UI thread: render + commit only)
 * ------------------------------------------------------------------ */

static ring_ent_t *ring_find(slot_t *s, uint32_t cp)
{
	for (uint32_t i = 0; i < RING_SLOTS; i++) {
		if (s->ring[i].cp == cp && s->ring[i].epoch == s->epoch) {
			return &s->ring[i];
		}
	}
	return NULL;
}

static void ring_reset(slot_t *s)
{
	for (uint32_t i = 0; i < RING_SLOTS; i++) {
		s->ring[i].cp = 0;
	}
	s->bump = 0;
	s->epoch = (uint8_t)(s->epoch + 1u);
}

/* bump+epoch arena: wrapping retires the whole epoch. Working-set policy
 * mirrors CrossPoint's overflow ring: one screen of text (~300-500
 * distinct glyphs, 4bpp CJK at 16 px = 106 B) lives in 48 KB; beyond that
 * the oldest epoch is dropped wholesale and re-warmed by the next render. */
static uint32_t ring_alloc(slot_t *s, uint32_t len)
{
	uint32_t need = (len + 3u) & ~3u;

	if (need > RING_BYTES) {
		return UINT32_MAX;
	}
	if (s->bump + need > RING_BYTES) {
		ring_reset(s);
		s_st.evictions++;
	}
	uint32_t off = s->bump;

	s->bump += need;
	return off;
}

static bool ring_put(slot_t *s, uint32_t cp, const uint8_t *src, uint32_t len)
{
	ring_ent_t *e = ring_find(s, cp);
	uint32_t off;

	if (e) {
		memcpy(s->arena + e->off, src, len);
		return true;
	}
	/* Entry exhaustion can happen before the byte arena fills (ASCII
	 * keyboard + settings). Retire the epoch in either case; otherwise a
	 * fetched glyph is discarded and can remain blank with no repaint. */
	for (uint32_t i = 0; i < RING_SLOTS; i++) {
		if (s->ring[i].cp == 0) {
			e = &s->ring[i];
			break;
		}
	}
	if (!e) {
		ring_reset(s);
		s_st.evictions++;
		e = &s->ring[0];
	}
	off = ring_alloc(s, len);
	if (off == UINT32_MAX) {
		return false;
	}
	memcpy(s->arena + off, src, len);
	e->cp = cp;
	e->off = off;
	e->epoch = s->epoch;
	return true;
}

/* ------------------------------------------------------------------ *
 * miss queue + worker (s_lock NEVER held across SD I/O)
 * ------------------------------------------------------------------ */

static void queue_miss_locked(uint8_t px, uint32_t cp)
{
	uint32_t n = (s_miss_t + MISS_Q - s_miss_h) % MISS_Q;
	uint32_t i;

	if (n >= MISS_Q - 1u) {
		return;			/* saturated: stays a blank box */
	}
	for (i = 0; i < n; i++) {
		uint32_t k = (s_miss_h + i) % MISS_Q;

		if (s_miss[k].px == px && s_miss[k].cp == cp) {
			return;		/* dedup */
		}
	}
	s_miss[s_miss_t].px = px;
	s_miss[s_miss_t].cp = cp;
	s_miss_t = (s_miss_t + 1u) % MISS_Q;
}

static void queue_miss(uint8_t px, uint32_t cp)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	queue_miss_locked(px, cp);
	k_mutex_unlock(&s_lock);
	k_work_schedule(&s_work, K_NO_WAIT);
}

static uint32_t pop_batch_locked(uint8_t *pxs, uint32_t *cps, uint32_t cap,
				 bool *more)
{
	uint32_t n = 0;

	*more = false;
	while (s_miss_h != s_miss_t) {
		miss_ent_t m = s_miss[s_miss_h];

		if (n == cap) {
			*more = true;
			break;
		}
		s_miss_h = (s_miss_h + 1u) % MISS_Q;
		pxs[n] = m.px;
		cps[n] = m.cp;
		n++;
	}
	return n;
}

/* Stream a slot's whole metric table (worker-owned mts; no lock needed).
 * The on-disk records are contiguous, so we open the file ONCE and read
 * METRIC_RECS-record chunks in a loop. The old per-chunk open/close made
 * ~98 SD opens across the 8 sizes and dominated cold boot (~7.5 s); one
 * open + sequential reads is what the whole resident-table set costs.
 * Partial progress is kept in mt_have, so a read error just retries the
 * remaining records from where we stopped on the next work pass. */
static void stream_metric(slot_t *s)
{
	uint32_t total_gi = s->gl_cnt;
	uint32_t first_gi = s->mt_have / sizeof(cp_mt_t);
	struct fs_file_t f;
	bool err = false;

	fs_file_t_init(&f);
	if (fs_open(&f, s->path, FS_O_READ) != 0) {
		s_io_fails++;
		return;			/* retry with poll backoff */
	}
	if (fs_seek(&f, (off_t)(s->rec_foff + first_gi * CPF_REC),
		    FS_SEEK_SET) != 0) {
		fs_close(&f);
		s_io_fails++;
		return;
	}
	while (first_gi < total_gi) {
		uint32_t n_gi = total_gi - first_gi;
		uint32_t recbytes;

		if (n_gi > METRIC_RECS) {
			n_gi = METRIC_RECS;
		}
		recbytes = n_gi * CPF_REC;
		if (aster_fs_read_safe(&f, s_burst, recbytes) != (int)recbytes) {
			err = true;
			break;
		}
		for (uint32_t k = 0; k < n_gi; k++) {
			cp_rec_t r;
			cp_mt_t *m = &s->mts[first_gi + k];

			rd_rec(&r, s_burst + k * CPF_REC);
			m->adv = r.adv;
			m->left = (int8_t)CLAMP((int)r.left, -128, 127);
			m->top = (int8_t)CLAMP((int)r.top, -128, 127);
			m->w = r.w;
			m->h = r.h;
			m->pad = 0;
		}
		first_gi += n_gi;
		s->mt_have += n_gi * sizeof(cp_mt_t);
		s_st.reads++;
	}
	fs_close(&f);
	if (err) {
		s_io_fails++;
		return;
	}
	s_io_fails = 0;
	LOG_INF("cpfont %upx: metrics streamed (%u glyphs)",
		(unsigned)s->px, s->gl_cnt);
}

/* stage one glyph from the burst temp (short lock) */
static bool stage_one(uint8_t px, uint32_t cp, const uint8_t *bmp,
		      uint32_t len)
{
	bool done = false;

	k_mutex_lock(&s_lock, K_FOREVER);
	for (uint32_t i = 0; i < STG_SLOTS; i++) {
		stg_t *st = &s_stg[i];

		if (st->used) {
			continue;
		}
		st->used = 1;
		st->px = px;
		st->cp = cp;
		st->len = (uint16_t)len;
		memcpy(st->bmp, bmp, len);
		done = true;
		break;
	}
	k_mutex_unlock(&s_lock);
	return done;
}

static void requeue(const uint8_t *pxs, const uint32_t *cps, uint32_t from,
		    uint32_t n)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	for (uint32_t k = from; k < n; k++) {
		queue_miss_locked(pxs[k], cps[k]);
	}
	k_mutex_unlock(&s_lock);
}

/* Resolve popped (px, cp) to (slot, gidx); sort by (px, gidx); walk
 * contiguous runs; per run: 1 open + record-span read + bitmap-span read. */
static void fetch_batch(const uint8_t *pxs, const uint32_t *cps, uint32_t n,
			bool *again)
{
	static slot_t *rs[BATCH_MAX] __attribute__((section(".ext_ram.bss.font_sd")));
	static uint32_t rg[BATCH_MAX] __attribute__((section(".ext_ram.bss.font_sd")));
	static uint32_t rc[BATCH_MAX] __attribute__((section(".ext_ram.bss.font_sd")));	/* original batch index */
	uint32_t m = 0;

	for (uint32_t i = 0; i < n; i++) {
		slot_t *s = slot_by_px(pxs[i]);
		int32_t gi;

		if (!s || s->state != ST_READY || !s->ivs) {
			continue;
		}
		gi = iv_find(s, cps[i]);
		if (gi < 0) {
			continue;	/* not in coverage: fallback font */
		}
		rs[m] = s;
		rg[m] = (uint32_t)gi;
		rc[m] = i;
		m++;
	}
	/* insertion sort by (px, gidx) */
	for (uint32_t i = 1; i < m; i++) {
		slot_t *ks = rs[i];
		uint32_t kg = rg[i], kc = rc[i];
		int32_t j = (int32_t)i - 1;

		while (j >= 0 && (rs[j]->px > ks->px ||
				  (rs[j]->px == ks->px && rg[j] > kg))) {
			rs[j + 1] = rs[j];
			rg[j + 1] = rg[j];
			rc[j + 1] = rc[j];
			j--;
		}
		rs[j + 1] = ks;
		rg[j + 1] = kg;
		rc[j + 1] = kc;
	}

	for (uint32_t a = 0; a < m; ) {
		slot_t *s = rs[a];
		uint32_t b = a + 1u;
		uint32_t cnt, recbytes, span_lo, span_hi, span;
		struct fs_file_t f;
		uint8_t *recs, *bmaps;
		bool ok;

		while (b < m && b - a < BURST_GLYPHS && rs[b] == s &&
		       rg[b] == rg[b - 1u] + 1u) {
			b++;
		}
		cnt = b - a;
		recbytes = cnt * CPF_REC;
		fs_file_t_init(&f);
		if (fs_open(&f, s->path, FS_O_READ) != 0) {
			s_io_fails++;
			requeue(pxs, cps, /* from */ rc[a], n);
			*again = true;
			return;
		}
		ok = fs_seek(&f, (off_t)(s->rec_foff + rg[a] * CPF_REC),
			     FS_SEEK_SET) == 0 &&
		     aster_fs_read_safe(&f, s_burst, recbytes) == (int)recbytes;
		if (!ok) {
			fs_close(&f);
			s_io_fails++;
			requeue(pxs, cps, rc[a], n);
			*again = true;
			return;
		}
		recs = s_burst;
		span_lo = UINT32_MAX;
		span_hi = 0;
		for (uint32_t k = 0; k < cnt; k++) {
			cp_rec_t r;

			rd_rec(&r, recs + k * CPF_REC);
			if (r.dlen == 0u) {
				continue;	/* space: advance only */
			}
			if (r.dlen > STG_BMP) {
				note_oversize(s->px, r.dlen, STG_BMP);
				continue;
			}
			if (r.doff < span_lo) {
				span_lo = r.doff;
			}
			if ((uint32_t)r.doff + r.dlen > span_hi) {
				span_hi = (uint32_t)r.doff + r.dlen;
			}
		}
		if (span_lo == UINT32_MAX) {
			fs_close(&f);	/* run of empty (space) glyphs */
			a = b;
			continue;
		}
		span = span_hi - span_lo;
		if (recbytes + span > BURST_BYTES) {
			fs_close(&f);
			a = b;		/* pathological; skip run */
			continue;
		}
		bmaps = s_burst + (BURST_BYTES - span);
		ok = fs_seek(&f, (off_t)(s->bm_foff + span_lo),
			     FS_SEEK_SET) == 0 &&
		     aster_fs_read_safe(&f, bmaps, span) == (int)span;
		fs_close(&f);
		if (!ok) {
			s_io_fails++;
			requeue(pxs, cps, rc[a], n);
			*again = true;
			return;
		}
		s_io_fails = 0;
		s_st.reads += 2;
		s_st.read_bytes += recbytes + span;
		for (uint32_t k = 0; k < cnt; k++) {
			cp_rec_t r;

			rd_rec(&r, recs + k * CPF_REC);
			if (r.dlen == 0u) {
				continue;	/* space: advance only */
			}
			if (r.dlen > STG_BMP) {
				note_oversize(s->px, r.dlen, STG_BMP);
				continue;
			}
			if (stage_one(s->px, cps[rc[a + k]],
				      bmaps + (r.doff - span_lo), r.dlen)) {
				s_st.warm_glyphs++;
			} else {
				queue_miss(s->px, cps[rc[a + k]]);
				*again = true;
			}
		}
		a = b;
	}
}

static void work_fn(struct k_work *w)
{
	static uint8_t pxs[BATCH_MAX] __attribute__((section(".ext_ram.bss.font_sd")));
	static uint32_t cps[BATCH_MAX] __attribute__((section(".ext_ram.bss.font_sd")));
	bool again = false, more = false;
	uint32_t n;
	bool streamed = false;

	ARG_UNUSED(w);
	atomic_set(&s_fetch_busy, 1);

	/* one metric chunk (SD I/O outside the lock) */
	for (uint32_t i = 0; i < ARRAY_SIZE(s_slots); i++) {
		slot_t *s = &s_slots[i];

		if (s->state == ST_TABLE &&
		    s->mt_have < sizeof(cp_mt_t) * s->gl_cnt) {
			stream_metric(s);
			streamed = true;
			break;
		}
	}

	k_mutex_lock(&s_lock, K_FOREVER);
	n = pop_batch_locked(pxs, cps, BATCH_MAX, &more);
	k_mutex_unlock(&s_lock);

	if (n) {
		fetch_batch(pxs, cps, n, &again);
	}

	if (more) {
		again = true;
	}
	if (again || streamed) {
		/* back off hard on repeated SD failures (card removed) */
		k_work_schedule(&s_work,
				s_io_fails > 3u ? K_MSEC(1000) : K_MSEC(5));
	} else {
		atomic_set(&s_fetch_busy, 0);
	}
}

/* ------------------------------------------------------------------ *
 * commit staging -> ring (UI thread)
 * ------------------------------------------------------------------ */

bool aster_font_sd_commit(void)
{
	bool dirty = false;

	/* Staging exists only while a reader/lyrics/chat session holds it.
	 * The launcher still commits every frame, and a NULL table is a load
	 * from address 0. */
	if (!s_stg) {
		return false;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	for (uint32_t i = 0; i < STG_SLOTS; i++) {
		stg_t *st = &s_stg[i];
		slot_t *s;

		if (!st->used) {
			continue;
		}
		s = slot_by_px(st->px);
		if (s && s->state == ST_READY && s->arena &&
		    !ring_find(s, st->cp)) {
			if (ring_put(s, st->cp, st->bmp, st->len)) {
				s_st.miss_filled++;
				dirty = true;
			}
		}
		st->used = 0;
		st->cp = 0;
	}
	k_mutex_unlock(&s_lock);
	if (dirty) {
		/* a caller that is about to paint sees the `dirty` return; one
		 * that is idle sees the latch at its next repaint_pending() */
		s_repaint = true;
		s_generation++;
	}
	return dirty;
}

/* ------------------------------------------------------------------ *
 * lv_font_t contract
 * ------------------------------------------------------------------ */

static bool sd_get_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *d,
		       uint32_t letter, uint32_t letter_next);
static const void *sd_get_bitmap(lv_font_glyph_dsc_t *d, lv_draw_buf_t *buf);

static void font_bind(slot_t *s)
{
	s->font.get_glyph_dsc = sd_get_dsc;
	s->font.get_glyph_bitmap = sd_get_bitmap;
	s->font.release_glyph = NULL;
	s->font.line_height = s->advY;
	s->font.base_line = (int32_t)s->advY - s->asc;
	s->font.subpx = LV_FONT_SUBPX_NONE;
	s->font.kerning = LV_FONT_KERNING_NONE;
	s->font.static_bitmap = 0;
	s->font.underline_position = -1;
	s->font.underline_thickness = 1;
	s->font.dsc = NULL;
	s->font.fallback = NULL;
}

static const struct sparse_ent *sparse_fetch(slot_t *s, uint32_t cp, int32_t gi);

static bool sd_get_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *d,
		       uint32_t letter, uint32_t letter_next)
{
	slot_t *s = CONTAINER_OF(font, slot_t, font);
	int32_t gi;
	const cp_mt_t *m;

	ARG_UNUSED(letter_next);
	if (letter < 0x20u || letter == 0xF8FFu || letter == 0x200Cu) {
		d->adv_w = 0;
		d->box_w = 0;
		d->box_h = 0;
		d->ofs_x = 0;
		d->ofs_y = 0;
		d->format = LV_FONT_GLYPH_FORMAT_NONE;
		d->is_placeholder = false;
		d->gid.index = 0;
		d->entry = NULL;
		return true;
	}
	if (s->state != ST_READY && s->state != ST_SPARSE) {
		return false;
	}
	gi = iv_find(s, letter);
	if (gi < 0) {
		return false;
	}
	if (s->state == ST_SPARSE) {
		const struct sparse_ent *e = sparse_fetch(s, letter, gi);

		if (!e) {
			return false;
		}
		d->adv_w = (uint16_t)((e->rec.adv + 8) >> 4);
		d->box_w = e->rec.w;
		d->box_h = e->rec.h;
		d->ofs_x = e->rec.left;
		d->ofs_y = (int16_t)((int)e->rec.top - (int)e->rec.h + 1);
		d->stride = 0;
		d->format = LV_FONT_GLYPH_FORMAT_A8;
		d->is_placeholder = false;
		d->gid.index = (uint32_t)gi;
		d->entry = NULL;
		return true;
	}
	if (!s->mts) {
		return false;
	}
	m = &s->mts[gi];
	d->adv_w = (uint16_t)((m->adv + 8) >> 4);	/* 12.4 -> px */
	d->box_w = m->w;
	d->box_h = m->h;
	d->ofs_x = m->left;
	/* LVGL's ofs_y is the offset from the baseline to the TOP of the box
	 * (y grows downward), i.e. -y2 = top - h + 1. */
	d->ofs_y = (int16_t)((int)m->top - (int)m->h + 1);
	d->stride = 0;
	d->format = LV_FONT_GLYPH_FORMAT_A8;
	d->is_placeholder = false;
	d->gid.index = (uint32_t)gi;
	d->entry = NULL;
	return true;
}

/* Hit: expand 4bpp ring bytes into the slot's A8 scratch. Covered but not
 * resident: correct-sized empty box + async fill; commit() repaints. */
static const void *sd_get_bitmap(lv_font_glyph_dsc_t *d, lv_draw_buf_t *buf)
{
	slot_t *s = CONTAINER_OF(d->resolved_font, slot_t, font);
	uint32_t gi = d->gid.index;
	ring_ent_t *e;
	const cp_mt_t *m;
	uint32_t stride, cp;

	ARG_UNUSED(buf);
	if (s->state == ST_SPARSE) {
		uint32_t cp = iv_cp(s, gi);
		const struct sparse_ent *e = cp ? sparse_fetch(s, cp, (int32_t)gi) : NULL;
		uint32_t stride;

		if (!e || e->rec.w == 0u || e->rec.h == 0u) {
			return NULL;
		}
		stride = lv_draw_buf_width_to_stride(e->rec.w, LV_COLOR_FORMAT_A8);
		if (stride * e->rec.h > SCRATCH_BYTES ||
		    (uint32_t)e->off + e->len > s_sparse_used) {
			return NULL;
		}
		for (uint32_t y = 0; y < e->rec.h; y++) {
			uint8_t *row = s_sparse_scratch + y * stride;
			uint32_t x;

			for (x = 0; x < e->rec.w; x++) {
				row[x] = pix2a(s_sparse_mem + e->off, y * e->rec.w + x,
					       s->two_bpp != 0u);
			}
			for (; x < stride; x++) {
				row[x] = 0;
			}
		}
		lv_draw_buf_init(&s->dbuf, e->rec.w, e->rec.h, LV_COLOR_FORMAT_A8, stride,
				 s_sparse_scratch, stride * e->rec.h);
		d->entry = NULL;
		return &s->dbuf;
	}
	if (s->state != ST_READY || !s->mts || !s->scratch ||
	    gi >= s->gl_cnt) {
		return NULL;
	}
	m = &s->mts[gi];
	if (m->w == 0u || m->h == 0u) {
		return NULL;	/* space: advance only */
	}
	stride = lv_draw_buf_width_to_stride(m->w, LV_COLOR_FORMAT_A8);
	if (stride * m->h > SCRATCH_BYTES) {
		note_oversize(s->px, stride * m->h, SCRATCH_BYTES);
		return NULL;	/* no room to expand it: draw nothing */
	}
	cp = iv_cp(s, gi);
	e = cp ? ring_find(s, cp) : NULL;
	if (e) {
		const uint8_t *src = s->arena + e->off;

		for (uint32_t y = 0; y < m->h; y++) {
			uint8_t *row = s->scratch + y * stride;
			uint32_t x;

			for (x = 0; x < m->w; x++) {
				row[x] = pix2a(src, y * m->w + x,
					       s->two_bpp != 0u);
			}
			for (; x < stride; x++) {
				row[x] = 0;
			}
		}
	} else {
		s_st.misses++;
		if (cp) {
			queue_miss(s->px, cp);
		}
		memset(s->scratch, 0, stride * m->h);
	}
	lv_draw_buf_init(&s->dbuf, m->w, m->h, LV_COLOR_FORMAT_A8, stride,
			 s->scratch, stride * m->h);
	d->entry = NULL;
	return &s->dbuf;
}

/* ------------------------------------------------------------------ *
 * header + intervals load (UI thread)
 * ------------------------------------------------------------------ */

static bool read_at(struct fs_file_t *f, uint32_t off, void *dst, uint32_t len)
{
	if (fs_seek(f, (off_t)off, FS_SEEK_SET) != 0) {
		return false;
	}
	return aster_fs_read_safe(f, dst, len) == (int)len;
}

static const struct sparse_ent *sparse_fetch(slot_t *s, uint32_t cp, int32_t gi)
{
	struct fs_file_t f;
	uint8_t raw[16];
	struct sparse_ent *e;
	cp_rec_t rec;

	if (!s_sparse_mem || gi < 0) {
		return NULL;
	}
	for (uint8_t i = 0; i < s_sparse_n; i++) {
		if (s_sparse[i].px == s->px && s_sparse[i].cp == cp) {
			return &s_sparse[i];
		}
	}
	fs_file_t_init(&f);
	if (fs_open(&f, s->path, FS_O_READ) != 0) {
		return NULL;
	}
	if (!read_at(&f, s->rec_foff + (uint32_t)gi * CPF_REC, raw, sizeof(raw))) {
		fs_close(&f);
		return NULL;
	}
	rd_rec(&rec, raw);
	if (s_sparse_n >= SPARSE_N ||
	    (uint32_t)s_sparse_used + rec.dlen > SPARSE_BYTES) {
		s_sparse_n = 0;
		s_sparse_used = 0;
	}
	if (rec.dlen > SPARSE_BYTES) {
		fs_close(&f);
		return NULL;
	}
	e = &s_sparse[s_sparse_n];
	if (rec.dlen &&
	    !read_at(&f, s->bm_foff + rec.doff, s_sparse_mem + s_sparse_used, rec.dlen)) {
		fs_close(&f);
		return NULL;
	}
	fs_close(&f);
	e->px = s->px;
	e->cp = cp;
	e->rec = rec;
	e->off = s_sparse_used;
	e->len = rec.dlen;
	s_sparse_used = (uint16_t)(s_sparse_used + rec.dlen);
	s_sparse_n++;
	s_st.reads += rec.dlen ? 2u : 1u;
	s_st.read_bytes += sizeof(raw) + rec.dlen;
	return e;
}

static void promote_full(slot_t *s)
{
	if (!s_want_full || s->state != ST_SPARSE || !s->ivs || s->mts ||
	    !s->arena || !s->scratch) {
		return;
	}
	s->mts = aster_psram_alloc(sizeof(cp_mt_t) * s->gl_cnt);
	if (!s->mts) {
		LOG_WRN("cpfont %upx: no PSRAM for metrics (%u B)",
			(unsigned)s->px, (unsigned)(sizeof(cp_mt_t) * s->gl_cnt));
		return;
	}
	s->mt_have = 0;
	s->state = ST_TABLE;
	k_work_schedule(&s_work, K_NO_WAIT);
	LOG_INF("cpfont %upx: streaming metrics for this session", (unsigned)s->px);
}

static int load_meta(slot_t *s)
{
	uint8_t hdr[64];
	uint8_t ivb[12];
	struct fs_file_t f;
	uint32_t ivc, glc, data_off;
	int r;

	fs_file_t_init(&f);
	r = fs_open(&f, s->path, FS_O_READ);
	if (r) {
		return r;
	}
	if (!read_at(&f, 0, hdr, 64) || memcmp(hdr, "CPFONT", 6) != 0 ||
	    hdr[6] != 0 || hdr[7] != 0 ||
	    (uint32_t)(hdr[8] | (hdr[9] << 8)) != CPF_VERSION) {
		LOG_WRN("cpfont %upx: bad header/version", (unsigned)s->px);
		fs_close(&f);
		s->state = ST_DEAD;
		return -EINVAL;
	}
	if (hdr[12] != 1u) {
		LOG_INF("cpfont %upx: %u styles, regular only",
			(unsigned)s->px, (unsigned)hdr[12]);
	}
	/* flags bit0 is the bitmap depth: 0 = 4bpp (what ASTER bakes), 1 = 2bpp.
	 * v4 has no way to say anything else -- an 8bpp file would set a bit we
	 * do not know and be read as if the rest were zero, which decodes to
	 * plausible grey mush rather than to an error. Reject unknown bits. */
	if ((hdr[10] & ~1u) != 0u) {
		LOG_WRN("cpfont %upx: unknown flags %#04x (depth beyond 2/4bpp "
			"needs a newer format)", (unsigned)s->px, (unsigned)hdr[10]);
		fs_close(&f);
		s->state = ST_DEAD;
		return -EINVAL;
	}
	s->two_bpp = (uint8_t)(hdr[10] & 1u);	/* flags bit0 */
	/* TOC[0] @32: styleId(1) pad(3) | ivc u32 @4 | glc u32 @8 |
	 * advY u8 @12 | asc i16 @13 | desc i16 @15 | kern.. @17..23 |
	 * dataOff u32 @24 | reserved @28..31 */
	ivc = ld32(hdr + 32 + 4);
	glc = ld32(hdr + 32 + 8);
	s->advY = hdr[32 + 12];
	s->asc = ld16s(hdr + 32 + 13);
	s->desc = ld16s(hdr + 32 + 15);
	data_off = ld32(hdr + 32 + 24);
	if (ivc == 0u || ivc > 20000u || glc == 0u || glc > 65536u ||
	    data_off != 64u || s->advY == 0u) {
		fs_close(&f);
		s->state = ST_DEAD;
		return -EINVAL;
	}
	if ((uint64_t)data_off + 12u * ivc > s->fsize ||
	    (uint64_t)data_off + 12u * ivc + (uint64_t)CPF_REC * glc >
		    s->fsize) {
		fs_close(&f);
		s->state = ST_DEAD;
		return -EINVAL;
	}
	if (!read_at(&f, data_off + 12u * (ivc - 1u), ivb, 12)) {
		fs_close(&f);
		s->state = ST_DEAD;
		return -EIO;
	}
	if (ld32(ivb + 8) + (ld32(ivb + 4) - ld32(ivb)) >= glc) {
		fs_close(&f);
		s->state = ST_DEAD;
		return -EINVAL;
	}
	s->ivs = aster_psram_alloc(12u * ivc);
	if (!s->ivs) {
		LOG_WRN("cpfont %upx: no PSRAM for ivs (%u B)",
			(unsigned)s->px, (unsigned)(12u * ivc));
		fs_close(&f);
		return -ENOMEM;
	}
	if (!read_at(&f, data_off, s->ivs, 12u * ivc)) {
		aster_psram_free(s->ivs);
		s->ivs = NULL;
		fs_close(&f);
		s->state = ST_DEAD;
		return -EIO;
	}
	fs_close(&f);
	s->gl_cnt = glc;
	s->iv_cnt = ivc;
	s->rec_foff = data_off + 12u * ivc;
	s->bm_foff = s->rec_foff + CPF_REC * glc;
	s->mt_have = 0;
	if (!s_want_full || !s->arena || !s->scratch) {
		font_bind(s);
		s->state = ST_SPARSE;
		LOG_INF("cpfont %upx: %u glyphs, intervals only",
			(unsigned)s->px, glc);
		return 0;
	}
	s->mts = aster_psram_alloc(sizeof(cp_mt_t) * glc);
	if (!s->mts) {
		LOG_WRN("cpfont %upx: no PSRAM for mts (%u B)",
			(unsigned)s->px, (unsigned)(sizeof(cp_mt_t) * glc));
		font_bind(s);
		s->state = ST_SPARSE;
		return 0;
	}
	s->state = ST_TABLE;
	s_st.reads += 2;
	k_work_schedule(&s_work, K_NO_WAIT);
	LOG_INF("cpfont %upx: %u glyphs %u ivals, streaming",
		(unsigned)s->px, glc, ivc);
	return 0;
}

/* ------------------------------------------------------------------ *
 * public API
 * ------------------------------------------------------------------ */

void aster_font_sd_init(void)
{
	k_mutex_init(&s_lock);
	k_work_init_delayable(&s_work, work_fn);
	s_sparse_mem = aster_psram_alloc(SPARSE_BYTES);
	if (!s_sparse_mem) {
		LOG_WRN("cpfont: no PSRAM for the %u B glyph cache", SPARSE_BYTES);
	}
	for (uint32_t i = 0; i < ARRAY_SIZE(s_baked_px); i++) {
		slot_t *s = &s_slots[i];

		s->px = s_baked_px[i];
		s->state = ST_EMPTY;
		s->epoch = 1;
		snprintf(s->path, sizeof(s->path), CP_DIR "/ui_%u.cpfont",
			 (unsigned)s->px);
	}
	s_probe_ms = 0;
	s_inited = true;
}

int aster_font_sd_poll(void)
{
	uint32_t now = k_uptime_get_32();
	int ready_edge = 0;
	struct fs_dirent st;

	if (!s_inited) {
		return 0;
	}
	/* flip finished streams to READY (UI thread owns state) */
	for (uint32_t i = 0; i < ARRAY_SIZE(s_slots); i++) {
		slot_t *s = &s_slots[i];

		if (s->state != ST_TABLE ||
		    s->mt_have < sizeof(cp_mt_t) * s->gl_cnt) {
			continue;
		}
		font_bind(s);
		s->state = ST_READY;
		LOG_INF("cpfont %upx READY", (unsigned)s->px);
		ready_edge = 1;
		s_repaint = true;	/* this face just became drawable */
		s_generation++;
	}
	/* Discovery. First sweep loads every baked file at once so the 48px
	 * clock size is READY within the same second as 12px -- the old
	 * one-stat-per-2s-window loop left the biggest sizes (the ones the
	 * launcher clock actually needs) undiscovered for ~16 s, and since
	 * launcher idle-pump shares the app worker thread, the first
	 * `aster app start` starved past its 15 s ready timeout.
	 * After the first sweep, fall back to a throttled single-slot re-probe
	 * so a re-plugged card / late-appearing file is still picked up. */
	if (!s_probe_first || (now - s_probe_ms) >= PROBE_MS) {
		bool sweep = !s_probe_first;
		s_probe_first = true;
		s_probe_ms = now;
		for (uint32_t i = 0; i < ARRAY_SIZE(s_slots); i++) {
			slot_t *s = &s_slots[i];

			if (s->state != ST_EMPTY && s->state != ST_DEAD) {
				continue;
			}
			if (fs_stat(s->path, &st) == 0 &&
			    st.type == FS_DIR_ENTRY_FILE && st.size > 96) {
				s->fsize = (uint32_t)st.size;
				load_meta(s);
			} else if (s->state == ST_DEAD) {
				s->state = ST_EMPTY;
			}
			if (!sweep) {
				break;	/* re-probe: one discovery per window */
			}
		}
	}
	if (s_want_full) {
		for (uint32_t i = 0; i < ARRAY_SIZE(s_slots); i++) {
			promote_full(&s_slots[i]);
		}
	}
	return ready_edge;
}

/* Any slot still pulling its metric table from SD? The launcher holds its
 * first paint while this is true (bounded), because until it flips there is
 * no font at all -- the paint would be blank rather than merely late. */
bool aster_font_sd_streaming(void)
{
	for (uint32_t i = 0; i < ARRAY_SIZE(s_slots); i++) {
		if (s_slots[i].state == ST_TABLE) {
			return true;
		}
	}
	return false;
}

bool aster_font_sd_busy(void)
{
	bool queued;

	k_mutex_lock(&s_lock, K_FOREVER);
	queued = s_miss_h != s_miss_t;
	k_mutex_unlock(&s_lock);
	return queued || atomic_get(&s_fetch_busy);
}

/* Repaint signal for guests that do not pump the pipeline themselves (the
 * launcher calls aster_ui_fonts_poll itself; a plain gfx app does not).
 *
 * Why this is not just a flag read: the fills only reach the ring from
 * aster_font_sd_commit(), which until now ran solely inside aster_gfx_exec_into
 * -- i.e. only when the app was already drawing. A settled app that paints
 * once per keypress never calls it, so a bitmap it asked for would sit in
 * staging forever and the blank box would never be corrected. This call
 * therefore ADVANCES both stages first, then reports.
 *
 * The answer is "is anything going to change the picture", not "is work
 * outstanding": it goes false as soon as the caller has drawn, so it is
 * safe to gate a repaint on it directly. A card that dies mid-fetch never
 * commits, so it never latches, and a caller cannot spin on it.
 *
 * Reading it is idempotent -- the latch is cleared by the DRAW
 * (aster_gfx_exec_into, i.e. aster_font_sd_repaint_ack), not by the read. */
bool aster_font_sd_repaint_pending(void)
{
	if (!s_inited) {
		return false;
	}
	(void)aster_font_sd_poll();
	(void)aster_font_sd_commit();
	return s_repaint;
}

uint32_t aster_font_sd_generation(void)
{
	return s_generation;
}

/* Drop the repaint request -- called from the draw entry point, which is
 * about to paint everything the pipeline has, so there is nothing left to
 * ask for. Deliberately NOT done inside the query above: if reading the
 * answer consumed it, a caller that asked twice in one frame would swallow
 * its own signal (which is exactly what a diagnostic log line did). */
void aster_font_sd_repaint_ack(void)
{
	s_repaint = false;
}

/* Boot-splash progress 0..100: every slot weighs the same; a streaming
 * slot contributes its metric-stream fraction, READY/DEAD slots are done.
 * Cheap (no locks: mt_have is a plain uint32 word read). */
uint32_t aster_font_sd_progress(void)
{
	uint32_t acc = 0;

	for (uint32_t i = 0; i < ARRAY_SIZE(s_slots); i++) {
		slot_t *s = &s_slots[i];
		uint32_t total;

		switch (s->state) {
		case ST_READY:
		case ST_SPARSE:
		case ST_DEAD:
			acc += 100u;
			break;
		case ST_TABLE:
			total = sizeof(cp_mt_t) * s->gl_cnt;
			if (total != 0u) {
				acc += (uint32_t)(((uint64_t)s->mt_have * 100u) /
						  total);
			}
			break;
		default:
			break;
		}
	}
	return acc / ARRAY_SIZE(s_slots);
}

const lv_font_t *aster_font_sd_get(uint8_t px)
{
	slot_t *s = slot_by_px(px);

	if (!s || (s->state != ST_READY && s->state != ST_SPARSE)) {
		return NULL;
	}
	return &s->font;
}

bool aster_font_sd_has_cp(uint8_t px, uint32_t cp)
{
	slot_t *s = slot_by_px(px);

	if (!s || (s->state != ST_READY && s->state != ST_SPARSE) || !s->ivs) {
		return false;
	}
	return iv_find(s, cp) >= 0;
}

void aster_font_sd_warm(uint8_t px, const char *utf8)
{
	slot_t *s = slot_by_px(px);
	const uint8_t *p;
	uint32_t n = 0;
	bool any = false;

	if (!s || (s->state != ST_READY && s->state != ST_SPARSE) || !s->ivs || !utf8) {
		return;
	}
	if (s->state == ST_SPARSE) {
		p = (const uint8_t *)utf8;
		while (*p != '\0') {
			uint32_t cp = utf8_next(&p);
			int32_t gi;

			if (cp == 0u) {
				break;
			}
			gi = iv_find(s, cp);
			if (gi >= 0) {
				(void)sparse_fetch(s, cp, gi);
			}
		}
		return;
	}
	p = (const uint8_t *)utf8;
	k_mutex_lock(&s_lock, K_FOREVER);
	while (*p != '\0' && n < MISS_Q) {
		uint32_t cp = utf8_next(&p);

		n++;
		if (cp == 0u) {
			break;
		}
		if (iv_find(s, cp) >= 0 && !ring_find(s, cp)) {
			queue_miss_locked(px, cp);
			any = true;
		}
	}
	k_mutex_unlock(&s_lock);
	if (any) {
		s_st.warm_bursts++;
		k_work_schedule(&s_work, K_NO_WAIT);
	}
}

void aster_font_sd_stats(aster_font_sd_stats_t *out)
{
	if (out) {
		*out = s_st;
	}
}

void aster_font_sd_cache_drop(void)
{
	s_sparse_n = 0;
	s_sparse_used = 0;
}

void aster_font_sd_session_begin(void)
{
	if (!s_inited) {
		return;
	}
	if (s_full_refs++ != 0u) {
		return;
	}
	if (!s_stg) {
		s_stg = aster_psram_alloc(sizeof(*s_stg) * STG_SLOTS);
		if (s_stg) {
			memset(s_stg, 0, sizeof(*s_stg) * STG_SLOTS);
		}
	}
	if (!s_burst) {
		s_burst = aster_psram_alloc(BURST_BYTES);
	}
	for (uint32_t i = 0; i < ARRAY_SIZE(s_slots); i++) {
		slot_t *s = &s_slots[i];

		if (!s->arena) {
			s->arena = aster_psram_alloc(RING_BYTES);
		}
		if (!s->scratch) {
			s->scratch = aster_psram_alloc(SCRATCH_BYTES);
		}
	}
	s_want_full = s_burst != NULL && s_stg != NULL;
	if (!s_want_full) {
		LOG_WRN("cpfont: full face buffers unavailable");
	}
	LOG_INF("cpfont session: metric tables on demand");
	(void)aster_font_sd_poll();
}

void aster_font_sd_session_end(void)
{
	struct k_work_sync sync;

	if (s_full_refs == 0u) {
		return;
	}
	if (--s_full_refs != 0u) {
		return;
	}
	s_want_full = false;
	(void)k_work_cancel_delayable_sync(&s_work, &sync);
	for (uint32_t i = 0; i < ARRAY_SIZE(s_slots); i++) {
		slot_t *s = &s_slots[i];

		aster_psram_free(s->mts);
		aster_psram_free(s->arena);
		aster_psram_free(s->scratch);
		s->mts = NULL;
		s->arena = NULL;
		s->scratch = NULL;
		s->mt_have = 0;
		ring_reset(s);
		if (s->ivs) {
			font_bind(s);
			s->state = ST_SPARSE;
		} else if (s->state != ST_DEAD) {
			s->state = ST_EMPTY;
		}
	}
	aster_psram_free(s_burst);
	aster_psram_free(s_stg);
	s_burst = NULL;
	s_stg = NULL;
	aster_font_sd_cache_drop();
	LOG_INF("cpfont session: metric tables released");
}
