"""Run actual sprite ownership and clipped raster code against bounded host stubs."""
from pathlib import Path
import unittest
from test_waveshare import ROOT,run_c

PREFIX=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "GFX_HEADER"
#include "ERROR_HEADER"
static int live,fail_alloc,fail_decode;
static void *aster_psram_alloc(size_t size){if(fail_alloc)return NULL;void *p=malloc(size);if(p)live++;return p;}
static void aster_psram_free(void *p){if(p){live--;free(p);}}
typedef struct {uint32_t out_w,out_h;bool unsupported,too_big;} aster_imgdec_info_t;
static int aster_imgdec_probe(const char *p,aster_imgdec_info_t *i,uint32_t w,uint32_t h){(void)p;*i=(aster_imgdec_info_t){.out_w=w,.out_h=h};return 0;}
static int aster_imgdec_render(const char *p,uint8_t *b,uint32_t w,uint32_t h,uint32_t *ow,uint32_t *oh,bool scale){(void)p;(void)scale;if(fail_decode)return ASTER_ECORRUPT;for(uint32_t n=0;n<w*h;n++)((uint16_t*)b)[n]=(uint16_t)(n+1);*ow=w;*oh=h;return 0;}
typedef struct {uint16_t *px;uint32_t w,h,pitch;} ppg_target_t;
typedef struct {uint32_t x0,y0,x1,y1;bool has;} acc_t;
static uint32_t s_clip_x,s_clip_y,s_clip_w,s_clip_h;
static uint16_t *row_at(ppg_target_t *t,uint32_t y){return t->px+y*t->pitch;}
static bool s_out_be;
static uint16_t out_pack(uint16_t c){return s_out_be?(uint16_t)((c>>8)|(c<<8)):c;}
static int blends;
static uint16_t out_blend(uint16_t d,uint16_t c,uint32_t a){(void)d;blends++;return c^(uint16_t)a;}
static void acc_merge(acc_t *a,uint32_t x,uint32_t y,uint32_t xx,uint32_t yy){*a=(acc_t){x,y,xx,yy,true};}
static uint32_t cmd_xy0(const aster_gfx_cmd_t *c){return (uint32_t)ASTER_GFX_P16(c->w[1]);}
static uint32_t cmd_xy1(const aster_gfx_cmd_t *c){return (uint32_t)ASTER_GFX_P16H(c->w[1]);}
'''

class GfxResources(unittest.TestCase):
 def code(self):
  source=(ROOT/'systems/zephyr/ui/ui_gfx.c').read_text()
  clip=source[source.index('static inline bool clip_rect('):source.index('/* Every op that names a point')]
  sprites=source[source.index('#define SPRITE_SLOTS'):source.index('/* ---------------- public entry ---------------- */')]
  return PREFIX.replace('GFX_HEADER',str(ROOT/'contract/include/aster/gfx.h')).replace('ERROR_HEADER',str(ROOT/'contract/include/aster/error.h'))+clip+sprites
 def test_live_handles_survive_exhaustion_failed_load_and_slot_reuse(self):
  run_c(self.code()+r'''
int main(void){
 (void)s_last_w;(void)s_last_h;(void)s_last_pitch;(void)s_last_be;
 uint32_t ids[6],w,h,next=0;
 for(int i=0;i<6;i++)assert(aster_gfx_bmp_load_into("a",2,2,&ids[i],&w,&h)==0);
 assert(live==6 && aster_gfx_bmp_load_into("b",2,2,&next,&w,&h)==ASTER_ENOMEM);
 for(int i=0;i<6;i++)assert(sprite_lookup(ids[i]));
 assert(aster_gfx_bmp_release(ids[0])==0);
 assert(aster_gfx_bmp_load_into("b",2,2,&next,&w,&h)==0 && next!=ids[0]);
 assert(!sprite_lookup(ids[0]) && aster_gfx_bmp_release(ids[0])==ASTER_EBADF);
 assert(sprite_lookup(next));assert(aster_gfx_bmp_release(next)==0);
 fail_decode=1;assert(aster_gfx_bmp_load_into("bad",2,2,&next,&w,&h)==ASTER_ECORRUPT);assert(live==5);
 fail_decode=0;fail_alloc=1;assert(aster_gfx_bmp_load_into("bad",2,2,&next,&w,&h)==ASTER_ENOMEM);assert(live==5);
 for(int i=1;i<6;i++)assert(sprite_lookup(ids[i]));
 fail_alloc=0;s_spr_sequence=UINT32_MAX;
 assert(aster_gfx_bmp_load_into("a",2,2,&next,&w,&h)==ASTER_ENOMEM);
 aster_gfx_drop_all();assert(live==0);for(int i=0;i<6;i++)assert(!sprite_lookup(ids[i]));return 0;}
''')
 def test_scaled_alpha_sprite_never_writes_outside_clip_or_pitch(self):
  run_c(self.code()+r'''
int main(void){
 (void)s_last_w;(void)s_last_h;(void)s_last_pitch;(void)s_last_be;
 uint32_t id,w,h;assert(aster_gfx_bmp_load_into("a",2,2,&id,&w,&h)==0);
 uint16_t pixels[24];for(int i=0;i<24;i++)pixels[i]=0xabcd;
 ppg_target_t t={pixels,4,4,6};acc_t a={0};
 s_clip_x=s_clip_y=1;s_clip_w=s_clip_h=2;
 aster_gfx_cmd_t c={{ASTER_GFX_BMP_SA,ASTER_GFX_PACK(-1,-1),id,0,512,128}};
 op_bmp(&t,&c,&a);assert(blends==4 && a.x0==1 && a.y0==1 && a.x1==3 && a.y1==3);
 for(int y=0;y<4;y++)for(int x=0;x<6;x++){
  if(x>=1&&x<3&&y>=1&&y<3)assert(pixels[y*6+x]==(4^128));
  else assert(pixels[y*6+x]==0xabcd);
 }
 s_clip_x=10;blends=0;a.has=false;op_bmp(&t,&c,&a);assert(blends==0&&!a.has);
 aster_gfx_drop_all();assert(live==0);return 0;}
''')

 def test_stale_resource_preflight_rejects_whole_batch(self):
  source=(ROOT/'systems/zephyr/ui/ui_gfx.c').read_text()
  begin=source.index('/* Validate resource references before any command changes pixels. */')
  end=source.index('set_out_be(out_be);',begin)
  validation=source[begin:end]
  run_c(self.code()+'\nstatic int writes;\nstatic int submit(const aster_gfx_cmd_t *cmds,uint32_t n){\n'+validation+'writes++;return 0;}\n'+r'''
int main(void){
 (void)s_last_w;(void)s_last_h;(void)s_last_pitch;(void)s_last_be;
 uint32_t id;assert(sprite_alloc(1,1,&id)==0);
 aster_gfx_cmd_t commands[2]={{{ASTER_GFX_CLEAR}},{{ASTER_GFX_BMP,0,id}}};
 assert(submit(commands,2)==0&&writes==1);
 assert(aster_gfx_bmp_release(id)==0);writes=0;
 assert(submit(commands,2)==ASTER_EBADF&&writes==0);
 return 0;}
''')

 def test_opaque_row_fast_path_preserves_pixels_and_byte_order(self):
  run_c(self.code()+r'''
int main(void){
 (void)s_last_w;(void)s_last_h;(void)s_last_pitch;(void)s_last_be;
 uint32_t id,w,h;assert(aster_gfx_bmp_load_into("a",2,2,&id,&w,&h)==0);
 assert(sprite_lookup(id)->opaque);
 uint16_t pixels[24];ppg_target_t t={pixels,4,4,6};acc_t a={0};
 aster_gfx_cmd_t c={{ASTER_GFX_BMP,ASTER_GFX_PACK(1,1),id}};
 for(int order=0;order<2;order++){
  s_out_be=order;for(int i=0;i<24;i++)pixels[i]=0xabcd;
  op_bmp(&t,&c,&a);assert(blends==0);
  for(int y=0;y<4;y++)for(int x=0;x<6;x++){
   if(x>=1&&x<3&&y>=1&&y<3)assert(pixels[y*6+x]==out_pack((uint16_t)((y-1)*2+x)));
   else assert(pixels[y*6+x]==0xabcd);
  }
 }
 aster_gfx_drop_all();assert(live==0);return 0;}
''')
