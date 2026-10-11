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

    def test_adjacent_dynamic_captions_do_not_reload_the_working_set(self):
        s = (ROOT / 'systems/zephyr/ui/font_sd.c').read_text()
        constants = '\n'.join(re.findall(r'^#define SPARSE_(?:BYTES|N)\s+.+$', s, re.M))
        start = s.index('static const struct sparse_ent *sparse_fetch(slot_t *s, uint32_t cp, int32_t gi)\n{')
        implementation = s[start:s.index('static void promote_full', start)]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#define FS_O_READ 1
#define CPF_REC 16
''' + constants + r'''
typedef struct {uint32_t doff;uint16_t dlen;} cp_rec_t;
typedef struct {uint8_t px;const char *path;uint32_t rec_foff,bm_foff;} slot_t;
struct fs_file_t {int px;};
struct sparse_ent {uint8_t px;uint32_t cp;cp_rec_t rec;uint16_t off,len;};
static uint8_t cache[SPARSE_BYTES];
static uint8_t *s_sparse_mem=cache;
static uint8_t s_sparse_n;
static uint16_t s_sparse_used;
static struct sparse_ent s_sparse[SPARSE_N];
static struct {unsigned reads,read_bytes;} s_st;
static unsigned disk_reads;
static void fs_file_t_init(struct fs_file_t *f) {memset(f,0,sizeof(*f));}
static int fs_open(struct fs_file_t *f,const char *p,int flags) {assert(flags==1);f->px=atoi(p);return 0;}
static void fs_close(struct fs_file_t *f) {(void)f;}
static bool read_at(struct fs_file_t *f,unsigned off,void *buf,unsigned len) {
 disk_reads++;
 if(len==16) {uint32_t index=(off-32)/CPF_REC;memcpy(buf,&index,4);}
 else {assert(len==64 && off>=100000);memset(buf,(off-100000)/64+f->px,len);}
 return true;
}
static void rd_rec(cp_rec_t *r,const uint8_t raw[16]) {
 uint32_t index;memcpy(&index,raw,4);r->doff=index*64;r->dlen=64;
}
''' + implementation + r'''
int main(void) {
 slot_t faces[2]={{.px=20,.path="20",.rec_foff=32,.bm_foff=100000},
                  {.px=16,.path="16",.rec_foff=32,.bm_foff=100000}};
 /* Two adjacent captions/titles use 100 distinct (size, codepoint) keys.
  * Revisiting either card must read zero new SD bytes. The old 48-slot
  * cache cleared itself partway through this working set. */
 for(int pass=0;pass<3;pass++) {
  unsigned before=disk_reads;
  for(int i=0;i<100;i++) {
   slot_t *f=&faces[i/50];int gi=i%50;
   const struct sparse_ent *e=sparse_fetch(f,0x4e00+gi,gi);
   assert(e && e->px==f->px && e->cp==0x4e00u+(unsigned)gi);
   assert(e->len==64 && cache[e->off]==gi+f->px);
  }
  if(pass) assert(disk_reads==before);
 }
 assert(s_sparse_n==100 && s_sparse_used==6400);
 assert(!sparse_fetch(faces,0x1234,-1));
 return 0;
}
''')
