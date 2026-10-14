"""Exercise the actual sparse SD cache with a mixed-size launcher working set."""
import re
import unittest
from test_waveshare import ROOT, run_c


class SparseCache(unittest.TestCase):
    def test_unchanged_app_caption_does_not_request_another_warm(self):
        s = (ROOT / 'systems/zephyr/ui/launcher/flux.c').read_text()
        helper = s[s.index('static void set_sd_text'):s.index('static void caption(void)')]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef struct {int px;} lv_font_t;
typedef struct {const char *text;const lv_font_t *font;} lv_obj_t;
static lv_font_t placeholder={0},generic={16};
static int ready,warm_calls;
static const lv_font_t *aster_ui_font(int px) {assert(px==16);return ready?&generic:&placeholder;}
static const char *lv_label_get_text(lv_obj_t *o) {return o->text;}
static const lv_font_t *lv_obj_get_style_text_font(lv_obj_t *o,int part) {(void)part;return o->font;}
static void aster_ui_font_warm(int px,const char *s) {assert(px==16 && s);warm_calls++;}
static void lv_obj_set_style_text_font(lv_obj_t *o,const lv_font_t *f,int part) {(void)part;o->font=f;}
static void lv_label_set_text(lv_obj_t *o,const char *s) {o->text=s;}
''' + helper + r'''
int main(void) {
 lv_obj_t label={.text="",.font=&placeholder};
 set_sd_text(&label,16,"包元数据");assert(warm_calls==1);
 for(int i=0;i<10;i++) set_sd_text(&label,16,"包元数据");
 assert(warm_calls==1);
 ready=1;set_sd_text(&label,16,"包元数据");
 assert(warm_calls==2 && label.font==&generic);
 set_sd_text(&label,16,"新的介绍");assert(warm_calls==3);
 return 0;
}
''')

    def test_per_size_cache_lru_and_lifetime(self):
        s=(ROOT/'systems/zephyr/ui/font_sd.c').read_text()
        constants='\n'.join(re.findall(r'^#define (?:SPARSE_(?:BYTES|N)|SCRATCH_BYTES)\s+.+$',s,re.M))
        structs=s[s.index('typedef struct {uint8_t w,h;'):s.index('static const uint8_t LUT2')]
        code=s[s.index('static bool cache_acquire'):s.index('static bool sd_get_dsc')]
        lifetime=s[s.index('void aster_font_sd_cache_drop'):s.index('bool aster_font_sd_cache_status')]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#define ASTER_FONT_MAX_SCRATCH 2352
#define CLAMP(v,l,h) ((v)<(l)?(l):(v)>(h)?(h):(v))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define LOG_INF(...) ((void)0)
#define FS_O_READ 1
#define CPF_REC 16
#define SPARSE_BYTES (48u*1024u)
#define SPARSE_N 192u
#define SCRATCH_BYTES ASTER_FONT_MAX_SCRATCH
typedef struct {int unused;} lv_draw_buf_t;
typedef struct {int unused;} lv_font_t;
''' +structs+r'''
static slot_t s_slots[2];
static bool s_inited=true;
static unsigned s_full_refs,disk_reads,live_allocs,fail_next;
static struct {unsigned reads,read_bytes,evictions,misses,miss_filled,hits,load_us,max_load_us;} s_st;
static uint32_t k_cycle_get_32(void){static uint32_t clock;return ++clock;}
static uint32_t k_cyc_to_us_floor64(uint32_t t){return t;}
static void *aster_psram_alloc(unsigned n) {if(fail_next && --fail_next==0) return NULL;void *p=malloc(n);if(p) live_allocs++;return p;}
static void aster_psram_free(void *p) {if(p) {live_allocs--;free(p);}}
struct fs_file_t {int px;};
static void fs_file_t_init(struct fs_file_t *f) {memset(f,0,sizeof(*f));}
static int fs_open(struct fs_file_t *f,const char *p,int flags) {assert(flags==1);f->px=atoi(p);return 0;}
static void fs_close(struct fs_file_t *f) {(void)f;}
static bool read_at(struct fs_file_t *f,unsigned off,void *buf,unsigned len) {
 disk_reads++;
 if(len==16) {unsigned index=(off-32)/CPF_REC;memcpy(buf,&index,4);}
 else {assert(len==64 && off>=100000);memset(buf,((off-100000)/64+f->px)&255,len);}
 return true;
}
static void rd_rec(cp_rec_t *r,const uint8_t raw[16]) {
 uint32_t index;memcpy(&index,raw,4);memset(r,0,sizeof(*r));r->w=8;r->h=16;r->doff=index*64;r->dlen=64;
}
''' +code+lifetime+r'''
int main(void) {
 for(int i=0;i<2;i++) {s_slots[i].px=i?16:20;snprintf(s_slots[i].path,64,"%u",s_slots[i].px);s_slots[i].rec_foff=32;s_slots[i].bm_foff=100000;s_slots[i].fsize=1000000;}
 assert(!s_slots[0].cache && !live_allocs);
 for(unsigned fail=1;fail<=3;fail++) {fail_next=fail;assert(!sparse_fetch(s_slots,0x4e00,0) && !live_allocs && !s_slots[0].cache);}
 for(int pass=0;pass<3;pass++) {
  unsigned before=disk_reads;
  for(int i=0;i<100;i++) {
   slot_t *f=&s_slots[i/50];int gi=i%50;
   const struct sparse_ent *e=sparse_fetch(f,0x4e00+gi,gi);
   assert(e && e->px==f->px && e->cp==0x4e00u+(unsigned)gi);
   assert(e->len==64 && f->cache->mem[e->off]==gi+f->px);
  }
  if(pass) assert(disk_reads==before);
 }
 assert(s_slots[0].cache->n==50 && s_slots[1].cache->n==50 && live_allocs==6);
 assert(s_st.hits==200 && s_st.miss_filled==100 && s_st.load_us==100 && s_st.max_load_us==1);
 for(int gi=50;gi<500;gi++) {
  assert(sparse_fetch(s_slots,0x4e00,0));
  assert(sparse_fetch(s_slots,0x4e00+gi,gi));
 }
 unsigned before=disk_reads;
 assert(sparse_fetch(s_slots,0x4e00,0));assert(disk_reads==before && s_st.evictions>0);
 struct sparse_cache *c=s_slots[0].cache;
 for(unsigned i=0;i<c->n;i++) {
  struct sparse_ent *e=&c->entries[i];assert(e->off+e->len<=c->used);
  for(unsigned j=0;j<e->len;j++) assert(c->mem[e->off+j]==((e->cp-0x4e00+20)&255));
 }
 assert(!sparse_fetch(s_slots,0x1234,-1));
 aster_font_sd_session_begin();assert(!live_allocs && s_full_refs==1);
 assert(sparse_fetch(s_slots,0x4e00,0));assert(live_allocs==3);
 aster_font_sd_session_begin();aster_font_sd_session_end();assert(live_allocs==3);
 aster_font_sd_session_end();assert(!live_allocs && !s_slots[0].cache);
 assert(sparse_fetch(s_slots+1,0x4e00,0));aster_font_sd_cache_drop();assert(!live_allocs);
 return 0;
}
''')

    def test_accelerated_font_bake_matches_scalar_edge_sampling(self):
        import importlib.util,random
        for dependency in ('numpy','freetype','fontTools'):
            if importlib.util.find_spec(dependency) is None:
                self.skipTest('optional font bake dependency unavailable: '+dependency)
        spec=importlib.util.spec_from_file_location('cpfont_bake',ROOT/'tools/build_cpfont.py')
        bake=importlib.util.module_from_spec(spec);spec.loader.exec_module(bake)
        if bake.np is None: self.skipTest('optional numpy accelerator unavailable')
        numpy=bake.np;r=random.Random(42)
        for _ in range(80):
            w,h=r.randrange(1,45),r.randrange(1,45)
            args=(bytes(r.randrange(256) for _ in range(w*h)),w,h,r.randrange(-8,9),r.randrange(-8,50),r.randrange(-4,5),r.randrange(0,15),r.choice([1,2,4]))
            fast=bake.resample_anchored(*args)
            try:
                bake.np=None;slow=bake.resample_anchored(*args)
            finally: bake.np=numpy
            self.assertEqual(fast,slow)

    def test_large_complete_font_passes_streaming_file_size_gate(self):
        s=(ROOT/'systems/zephyr/services/aster_file_svc.c').read_text()
        define=re.search(r'^#define FSVC_MAX_FILE.*$',s,re.M)[0]
        a=s.index('static void do_put(');b=s.index('\n\tchar tmp[192];',a)
        gate=s[a:b].replace('{\n','{\n (void)dev;(void)path;(void)want_crc;(void)off;\n',1)+'\n}\n'
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>
struct device {int unused;};
static int verdict;
static void cdc_reply(const char *s,...) {va_list a;va_start(a,s);verdict=va_arg(a,int);va_end(a);}
static int aster_storage_mounted(void) {return 0;}
''' +define+'\n'+gate+r'''
int main(void) {
 do_put(NULL,"font",19586836,0,0);assert(verdict==-ENODEV);
 do_put(NULL,"font",27776000,0,0);assert(verdict==-ENODEV);
 do_put(NULL,"font",FSVC_MAX_FILE+1,0,0);assert(verdict==-EFBIG);
 return 0;
}
''')

    def test_native_hinting_keeps_metrics_and_grayscale_at_requested_size(self):
        import importlib.util
        for dependency in ('freetype','fontTools'):
            if importlib.util.find_spec(dependency) is None:
                self.skipTest('optional font bake dependency unavailable: '+dependency)
        spec=importlib.util.spec_from_file_location('bake_hint',ROOT/'tools/build_cpfont.py')
        bake=importlib.util.module_from_spec(spec);spec.loader.exec_module(bake)
        text=set(map(ord,'音乐设置清晰中文笔画ABCxyz012345'))
        base=bake.rasterize(ROOT/'resources/fonts/ui.ttf',12,text,bake.make_lut(1),4)
        hinted=bake.rasterize(ROOT/'resources/fonts/ui.ttf',12,text,bake.make_lut(1),4,12)
        self.assertEqual(base[1],hinted[1])
        self.assertEqual(base[4:7],hinted[4:7])
        self.assertEqual([g[2] for g in base[2]],[g[2] for g in hinted[2]])
        self.assertNotEqual(base[3],hinted[3])
        levels={v for packed in hinted[3] for byte in packed for v in (byte>>4,byte&15)}
        self.assertGreater(len(levels),8) # Grayscale, not monochrome thresholding.
        unchanged=bake.rasterize(ROOT/'resources/fonts/ui.ttf',16,text,bake.make_lut(1),4,12)
        self.assertEqual(unchanged,bake.rasterize(ROOT/'resources/fonts/ui.ttf',16,text,bake.make_lut(1),4))
