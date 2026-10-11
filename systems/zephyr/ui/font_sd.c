/* SPDX-License-Identifier: MIT
 * Full-coverage cpfont faces on SD; native pixels, on-demand PSRAM size caches.
 * Only interval metadata stays resident. Glyph records and packed bitmaps are
 * fetched together, retained by LRU, and released when the screen owner changes.
 * All callers run on the shared WAMR/LVGL worker; no background reader can race
 * cache teardown. Boot/app prewarm runs before first presentation.
 */
#include "font_sd.h"
#include "aster_mem.h"
#include "aster_storage.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aster_fontsd, LOG_LEVEL_INF);
#define CP_DIR "/SD:/aster/fonts"
#define CPF_VERSION 4u
#define CPF_REC 16u
#define PROBE_MS 2000u
#define SPARSE_BYTES (48u * 1024u)
#define SPARSE_N 192u
#define SCRATCH_BYTES ASTER_FONT_MAX_SCRATCH
BUILD_ASSERT(ASTER_FONT_MAX_BMP <= SPARSE_BYTES && SCRATCH_BYTES > 0);
BUILD_ASSERT(LV_DRAW_BUF_STRIDE_ALIGN == 1);
enum {ST_EMPTY,ST_SPARSE,ST_DEAD};
typedef struct {uint8_t w,h;uint16_t adv;int16_t left,top;uint16_t dlen;uint32_t doff;} cp_rec_t;
typedef struct {uint32_t first,last,offset;} cp_iv_t;
struct sparse_ent {uint8_t px;uint32_t cp;cp_rec_t rec;uint16_t off,len;uint32_t touched;};
struct sparse_cache {uint8_t *mem,*scratch;uint32_t capacity,used,clock;uint16_t n;struct sparse_ent entries[SPARSE_N];};
typedef struct {
 uint8_t px,state,two_bpp,advY;int16_t asc,desc;
 char path[64];uint32_t fsize,iv_cnt,gl_cnt,rec_foff,bm_foff;
 volatile uint32_t cache_bytes,cache_used,cache_glyphs;
 cp_iv_t *ivs;struct sparse_cache *cache;lv_draw_buf_t dbuf;lv_font_t font;
} slot_t;
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

static const uint8_t s_baked_px[]=ASTER_FONT_BAKED_PX;
static slot_t s_slots[ARRAY_SIZE(s_baked_px)] __attribute__((section(".ext_ram.bss.font_sd")));
static bool s_inited,s_probe_first,s_repaint;
static uint32_t s_probe_ms,s_generation,s_full_refs;
static aster_font_sd_stats_t s_st;
uint32_t aster_font_sd_face_n(void) {return ARRAY_SIZE(s_baked_px);}
uint8_t aster_font_sd_face_px(uint32_t i) {return i<ARRAY_SIZE(s_baked_px)?s_baked_px[i]:0;}
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

static bool read_at(struct fs_file_t *f, uint32_t off, void *dst, uint32_t len)
{
	if (fs_seek(f, (off_t)off, FS_SEEK_SET) != 0) {
		return false;
	}
	return aster_fs_read_safe(f, dst, len) == (int)len;
}

static bool cache_acquire(slot_t *s)
{
 if(s->cache) return true;
 struct sparse_cache *c=aster_psram_alloc(sizeof(*c));
 if(!c) return false;
 memset(c,0,sizeof(*c));
 c->capacity=CLAMP((uint32_t)s->px*s->px*48u,8192u,SPARSE_BYTES);
 c->mem=aster_psram_alloc(c->capacity);
 c->scratch=aster_psram_alloc(SCRATCH_BYTES);
 if(!c->mem || !c->scratch) {
  aster_psram_free(c->mem);aster_psram_free(c->scratch);aster_psram_free(c);
  return false;
 }
 s->cache=c;s->cache_bytes=sizeof(*c)+c->capacity+SCRATCH_BYTES;
 LOG_INF("cpfont cache: %upx allocated %u B",s->px,
         (unsigned)(sizeof(*c)+c->capacity+SCRATCH_BYTES));
 return true;
}

static void cache_evict(struct sparse_cache *c)
{
 uint16_t oldest=0;
 for(uint16_t i=1;i<c->n;i++)
  if(c->clock-c->entries[i].touched>c->clock-c->entries[oldest].touched) oldest=i;
 uint32_t off=c->entries[oldest].off,len=c->entries[oldest].len;
 memmove(c->mem+off,c->mem+off+len,c->used-off-len);
 for(uint16_t i=0;i<c->n;i++)
  if(c->entries[i].off>=off+len) c->entries[i].off-=len;
 c->entries[oldest]=c->entries[--c->n];c->used-=len;
 s_st.evictions++;
}

static const struct sparse_ent *sparse_fetch(slot_t *s, uint32_t cp, int32_t gi)
{
 struct fs_file_t f;uint8_t raw[16];cp_rec_t rec;
 if(gi<0 || !cache_acquire(s)) return NULL;
 struct sparse_cache *c=s->cache;
 for(uint16_t i=0;i<c->n;i++) if(c->entries[i].cp==cp) {
  c->entries[i].touched=++c->clock;return &c->entries[i];
 }
 s_st.misses++;
 fs_file_t_init(&f);
 if(fs_open(&f,s->path,FS_O_READ)!=0) return NULL;
 if(!read_at(&f,s->rec_foff+(uint32_t)gi*CPF_REC,raw,sizeof(raw))) {
  fs_close(&f);return NULL;
 }
 rd_rec(&rec,raw);
 if(rec.dlen>c->capacity || (uint32_t)rec.w*rec.h>SCRATCH_BYTES ||
    rec.dlen!=((uint32_t)rec.w*rec.h+(s->two_bpp?3u:1u))/(s->two_bpp?4u:2u) ||
    (uint64_t)s->bm_foff+rec.doff+rec.dlen>s->fsize) {
  fs_close(&f);return NULL;
 }
 while(c->n && (c->n>=SPARSE_N || c->used+rec.dlen>c->capacity)) cache_evict(c);
 if(rec.dlen && !read_at(&f,s->bm_foff+rec.doff,c->mem+c->used,rec.dlen)) {
  fs_close(&f);return NULL;
 }
 fs_close(&f);
 struct sparse_ent *e=&c->entries[c->n++];
 e->px=s->px;e->cp=cp;e->rec=rec;e->off=c->used;e->len=rec.dlen;
 e->touched=++c->clock;c->used+=rec.dlen;
 s->cache_used=c->used;s->cache_glyphs=c->n;s_st.miss_filled++;
 s_st.reads+=rec.dlen?2u:1u;s_st.read_bytes+=sizeof(raw)+rec.dlen;
 return e;
}

static bool sd_get_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *d,
		       uint32_t letter, uint32_t letter_next)
{
	slot_t *s = CONTAINER_OF(font, slot_t, font);
	int32_t gi;

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
	if (s->state != ST_SPARSE) {
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
	return false;
}

static const void *sd_get_bitmap(lv_font_glyph_dsc_t *d, lv_draw_buf_t *buf)
{
	slot_t *s = CONTAINER_OF(d->resolved_font, slot_t, font);
 uint32_t gi=d->gid.index;

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
		    !s->cache || (uint32_t)e->off + e->len > s->cache->used) {
			return NULL;
		}
		for (uint32_t y = 0; y < e->rec.h; y++) {
			uint8_t *row = s->cache->scratch + y * stride;
			uint32_t x;

			for (x = 0; x < e->rec.w; x++) {
				row[x] = pix2a(s->cache->mem + e->off, y * e->rec.w + x,
					       s->two_bpp != 0u);
			}
			for (; x < stride; x++) {
				row[x] = 0;
			}
		}
		lv_draw_buf_init(&s->dbuf, e->rec.w, e->rec.h, LV_COLOR_FORMAT_A8, stride,
				 s->cache->scratch, stride * e->rec.h);
		d->entry = NULL;
		return &s->dbuf;
	}

 return NULL;
}

static bool sd_get_dsc(const lv_font_t *,lv_font_glyph_dsc_t *,uint32_t,uint32_t);
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
 font_bind(s);s->state=ST_SPARSE;
 LOG_INF("cpfont %upx: %u glyphs available",s->px,glc);
 return 0;
}

void aster_font_sd_init(void)
{
 for(uint32_t i=0;i<ARRAY_SIZE(s_slots);i++) {
  s_slots[i].px=s_baked_px[i];
  snprintf(s_slots[i].path,sizeof(s_slots[i].path),CP_DIR "/ui_%u.cpfont",s_slots[i].px);
 }
 s_inited=true;
}
int aster_font_sd_poll(void)
{
 if(!s_inited) return 0;
 uint32_t now=k_uptime_get_32();int changed=0;struct fs_dirent st;
 if(s_probe_first && now-s_probe_ms<PROBE_MS) return 0;
 s_probe_first=true;s_probe_ms=now;
 for(uint32_t i=0;i<ARRAY_SIZE(s_slots);i++) {
  slot_t *s=&s_slots[i];
  if(s->state==ST_SPARSE) continue;
  if(fs_stat(s->path,&st)==0 && st.type==FS_DIR_ENTRY_FILE && st.size>96) {
   s->fsize=st.size;
   if(load_meta(s)==0) changed=1;
  }
 }
 if(changed) {s_repaint=true;s_generation++;}
 return changed;
}
bool aster_font_sd_streaming(void) {return false;}
bool aster_font_sd_busy(void) {return false;}
bool aster_font_sd_commit(void) {return false;}
bool aster_font_sd_repaint_pending(void) {aster_font_sd_poll();return s_repaint;}
void aster_font_sd_repaint_ack(void) {s_repaint=false;}
uint32_t aster_font_sd_generation(void) {return s_generation;}
uint32_t aster_font_sd_progress(void) {return s_probe_first?100:0;}
const lv_font_t *aster_font_sd_get(uint8_t px)
{
 slot_t *s=slot_by_px(px);
 if(!s || s->state!=ST_SPARSE || !cache_acquire(s)) return NULL;
 return &s->font;
}
bool aster_font_sd_has_cp(uint8_t px,uint32_t cp)
{
 slot_t *s=slot_by_px(px);
 return s && s->state==ST_SPARSE && iv_find(s,cp)>=0;
}
void aster_font_sd_warm(uint8_t px,const char *utf8)
{
 slot_t *s=slot_by_px(px);
 if(!s || s->state!=ST_SPARSE || !utf8) return;
 s_st.warm_bursts++;
 const uint8_t *p=(const uint8_t *)utf8;
 while(*p) {uint32_t cp=utf8_next(&p);int32_t gi=iv_find(s,cp);
  if(gi>=0 && sparse_fetch(s,cp,gi)) s_st.warm_glyphs++;
 }
}
void aster_font_sd_stats(aster_font_sd_stats_t *out) {if(out) *out=s_st;}
void aster_font_sd_cache_drop(void)
{
 unsigned freed=0;
 for(uint32_t i=0;i<ARRAY_SIZE(s_slots);i++) {
  struct sparse_cache *c=s_slots[i].cache;
  if(!c) continue;
  freed+=sizeof(*c)+c->capacity+SCRATCH_BYTES;
  aster_psram_free(c->mem);aster_psram_free(c->scratch);aster_psram_free(c);
  s_slots[i].cache=NULL;s_slots[i].cache_bytes=0;s_slots[i].cache_used=0;s_slots[i].cache_glyphs=0;
 }
 if(freed) LOG_INF("cpfont cache: released %u B",freed);
}

void aster_font_sd_session_begin(void)
{
 if(!s_inited || s_full_refs++!=0u) return;
 /* The launcher has been hidden; font objects stay stable, glyph data does not. */
 aster_font_sd_cache_drop();
 LOG_INF("cpfont app session: size caches on demand");
}

void aster_font_sd_session_end(void)
{
 if(!s_full_refs || --s_full_refs) return;
 aster_font_sd_cache_drop();
 LOG_INF("cpfont app session: caches released");
}

bool aster_font_sd_cache_status(uint32_t index,aster_font_cache_status_t *out)
{
 if(index>=ARRAY_SIZE(s_slots) || !out) return false;
 const slot_t *s=&s_slots[index];
 *out=(aster_font_cache_status_t){.px=s->px,.available=s->state==ST_SPARSE,
   .coverage=s->gl_cnt,.glyphs=s->cache_glyphs,.used=s->cache_used,.bytes=s->cache_bytes};
 return true;
}
