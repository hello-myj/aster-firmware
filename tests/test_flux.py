"""Exercise FLUX cyclic selection and app-edge return using the actual C helpers."""
import unittest
from test_waveshare import ROOT, run_c

class Flux(unittest.TestCase):
    def test_neighbor_cache_retains_existing_textures(self):
        source=(ROOT/'systems/zephyr/ui/launcher/flux.c').read_text()
        helper=source[source.index('static int cache_slot'):source.index('static void redraw(void)\n{')]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#define RIBBONS 5
''' +helper+r'''
int main(void) {
 int resident[5]={1,2,3,4,5},wanted[5]={2,3,4,5,6};
 for(int i=0;i<4;i++) assert(cache_slot(resident,wanted,5,wanted[i])==i+1);
 assert(cache_slot(resident,wanted,5,6)==0);
 resident[0]=6;
 assert(cache_slot(resident,wanted,5,6)==0);
 assert(cache_slot(resident,wanted,5,99)==-1);
 int across_wrap[5]={14,0,1,2,3},before_wrap[5]={0,1,2,3,4};
 assert(cache_slot(before_wrap,across_wrap,5,14)==4);
 assert(cache_slot(before_wrap,across_wrap,5,0)==0);
 int smaller[3]={2,3,4};
 assert(cache_slot(resident,smaller,3,2)==1);
 return 0;
}
''')

    def test_negative_wrap_and_edge_return(self):
        source=(ROOT/'systems/zephyr/ui/launcher/flux.c').read_text()
        cyclic=source[source.index('static int nearest'):source.index('static entry_t *current')]
        edge=source[source.index('static struct {bool down,edge;'):source.index('int aster_launcher_init')]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
static int width=320;
static int aster_ui_lvgl_width(void) {return width;}
''' +cyclic+edge+r'''
int main(void) {
 assert(nearest(-1000)==-1 && nearest(-600)==-1 && nearest(-400)==0);
 assert(nearest(600)==1 && wrap(-1,5)==4 && wrap(7,5)==2);
 assert(distance(4,0,5)==-1000 && distance(0,4000,5)==1000);
 assert(distance(0,0,0)==0 && wrap(1,0)==0);
 assert(!aster_launcher_app_touch(297,160,true));
 assert(!aster_launcher_app_touch(210,160,false));
 assert(!aster_launcher_app_touch(298,160,true));
 assert(!aster_launcher_app_touch(233,160,false));
 assert(!aster_launcher_app_touch(298,160,true));
 assert(aster_launcher_app_touch(232,165,false));
 assert(!aster_launcher_app_touch(232,165,false));
 assert(!aster_launcher_app_touch(319,160,true));
 assert(!aster_launcher_app_touch(230,270,false));
 width=480;
 assert(!aster_launcher_app_touch(457,160,true));
 assert(!aster_launcher_app_touch(300,160,false));
 assert(!aster_launcher_app_touch(458,160,true));
 assert(aster_launcher_app_touch(390,160,false));
 return 0;
}
''')

    def test_fused_draw_clips_blends_and_falls_back(self):
        source=(ROOT/'systems/zephyr/ui/flux_draw.c').read_text()
        implementation=source[source.index('static struct {const void *source;'):]
        math_source=(ROOT/'third_party/lvgl/src/misc/lv_math.c').read_text()
        table=math_source[math_source.index('static const uint16_t sin0_90_table'):math_source.index('/**********************\n *      MACROS')]
        trig=math_source[math_source.index('int32_t LV_ATTRIBUTE_FAST_MEM lv_trigo_sin'):math_source.index('/**\n * cubic-bezier')]
        run_c(r''' 
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#define LV_COLOR_FORMAT_RGB565 1
#define LV_COLOR_FORMAT_RGB565_SWAPPED 2
#define LV_BLEND_MODE_NORMAL 0
#define LV_OPA_MIN 2
#define LV_TRIGO_SHIFT 15
#define LV_RESULT_INVALID 1
#define LV_RESULT_OK 0
typedef struct {int x,y;} point;
typedef struct {int x1,y1,x2,y2;} lv_area_t;
typedef struct {uint8_t *data;struct {unsigned stride;} header;} buffer;
typedef struct {buffer *draw_buf;int color_format;lv_area_t buf_area;} lv_layer_t;
typedef struct {lv_layer_t *target_layer;lv_area_t clip_area;} lv_draw_task_t;
typedef struct {int antialias,recolor_opa,clip_radius,blend_mode,scale_x,scale_y,rotation,opa;void *colorkey;point pivot;} lv_draw_image_dsc_t;
static int lv_area_get_width(const lv_area_t *a) {return a->x2-a->x1+1;}
static int lv_area_get_height(const lv_area_t *a) {return a->y2-a->y1+1;}
static bool lv_area_intersect(lv_area_t *o,const lv_area_t *a,const lv_area_t *b) {
 lv_area_t r={a->x1>b->x1?a->x1:b->x1,a->y1>b->y1?a->y1:b->y1,
 a->x2<b->x2?a->x2:b->x2,a->y2<b->y2?a->y2:b->y2};*o=r;
 return r.x1<=r.x2 && r.y1<=r.y2;
}
#define LV_ATTRIBUTE_FAST_MEM
#define FLUX_FAST
static uint32_t k_cycle_get_32(void) {return 0;}
static uint32_t k_cyc_to_us_floor64(uint32_t v) {return v;}
static void *aster_psram_alloc(unsigned n) {return malloc(n);}
static void aster_psram_free(void *p) {free(p);}
''' +table+trig+implementation+r'''
int main(void) {
 static uint16_t src[360*140],dst[322*482];
 for(int i=0;i<360*140;i++) src[i]=0xf800;
 for(int i=0;i<322*482;i++) dst[i]=0xa55a;
 buffer buf={.data=(uint8_t *)(dst+323),.header={.stride=322*2}};
 lv_layer_t layer={.draw_buf=&buf,.color_format=2,.buf_area={0,0,319,479}};
 lv_draw_task_t task={.target_layer=&layer,.clip_area={20,30,199,99}};
 lv_area_t coords={-20,10,339,149},clip={0,0,319,479};
 lv_draw_image_dsc_t d={.scale_x=256,.scale_y=256,.opa=255,.pivot={180,70}};
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
 assert(dst[323+30*322+20]==0x00f8 && dst[323+99*322+199]==0x00f8);
 assert(dst[323+29*322+20]==0xa55a && dst[323+30*322+19]==0xa55a);
 assert(dst[0]==0xa55a && dst[323+480*322]==0xa55a);
 /* Half scale occupies x=70..249, y=45..114; outside stays untouched. */
 memset(dst,0,sizeof(dst));d.scale_x=d.scale_y=128;
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
 assert(dst[323+50*322+80]==0x00f8 && dst[323+40*322+80]==0);
 assert(dst[323+50*322+69]==0);
 /* 90 degree inverse mapping: width/height swap about the pivot. */
 memset(dst,0,sizeof(dst));d.scale_x=d.scale_y=256;d.rotation=900;
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
 assert(dst[323+50*322+100]==0x00f8 && dst[323+50*322+80]==0);
 d.rotation=0;d.opa=128;memset(dst,0,sizeof(dst));
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
 assert(dst[323+50*322+100]==swap16(0x7800));
 /* Coordinate-coded source checks rotation direction and a diagonal
  * fractional scale against independently calculated sample locations. */
 for(int i=0;i<360*140;i++) src[i]=(uint16_t)i;
 d.opa=255;d.rotation=900;
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
 assert(dst[323+50*322+100]==swap16(130*360+150));
 d.rotation=2700;
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
 assert(dst[323+50*322+100]==swap16(10*360+210));
 d.rotation=450;d.scale_x=d.scale_y=206;
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
 assert(dst[323+50*322+100]==swap16(96*360+101));
 d.antialias=1;d.rotation=0;d.scale_x=d.scale_y=512;d.pivot=(point){0,0};
 coords=(lv_area_t){20,10,379,149};task.clip_area=clip;
 memset(dst,0,sizeof(dst));for(int i=0;i<360*140;i++) src[i]=0xf800;
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
 assert(dst[323+20*322+18]==0);
 assert(dst[323+20*322+19]==swap16(0x7800));
 assert(dst[323+20*322+20]==swap16(0xf800));
 aster_flux_draw_prepare(src,720);
 memset(dst,0,sizeof(dst));
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
 assert(dst[323+20*322+19]==swap16(0x7800));
 assert(dst[323+20*322+20]==swap16(0xf800));
 aster_flux_draw_forget(src);
 assert(prepared[0].source==NULL && prepared[0].detail==NULL);
 /* Sparse content in several eight-row bands must produce identical
  * output with and without long uniform-margin skips, at either tilt. */
 static uint16_t reference[322*482];
 for(int y=18;y<125;y++) for(int x=91;x<210;x++)
  if((x+y)%7) src[y*360+x]=(uint16_t)(x*37+y*131);
 d.scale_x=d.scale_y=221;d.pivot=(point){180,70};d.opa=173;
 coords=(lv_area_t){-20,110,339,249};
 for(int angle=-140;angle<=140;angle+=280) {
  d.rotation=angle;
  memset(dst,0x25,sizeof(dst));
  assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
  memcpy(reference,dst,sizeof(dst));aster_flux_draw_prepare(src,720);
  memset(dst,0x25,sizeof(dst));
  assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
  assert(!memcmp(reference,dst,sizeof(dst)));aster_flux_draw_forget(src);
 }
 d.colorkey=src;
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==1);
 d.colorkey=NULL;
 d.antialias=0;layer.color_format=1;
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==1);
 return 0;
}
''')

    def test_native_fonts_raw_bitmap_wrapper_and_unknown_text(self):
        source=(ROOT/'systems/zephyr/ui/launcher/flux_font.c').read_text()
        implementation=source[source.index('typedef struct {'):]
        implementation=implementation.replace('#include "flux_fonts.inc"','#include "'+str(ROOT/'systems/zephyr/ui/launcher/flux_fonts.inc')+'"')
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#define LV_FONT_GLYPH_FORMAT_A8 8
#define LV_COLOR_FORMAT_A8 8
typedef struct {uint8_t *data;struct {unsigned w,h,stride;} header;} lv_draw_buf_t;
typedef struct font lv_font_t;
typedef struct {unsigned adv_w,box_w,box_h,stride,format;int ofs_x,ofs_y;bool is_placeholder,req_raw_bitmap;struct {unsigned index;} gid;void *entry;const lv_font_t *resolved_font;} lv_font_glyph_dsc_t;
struct font {
 bool (*get_glyph_dsc)(const lv_font_t *,lv_font_glyph_dsc_t *,uint32_t,uint32_t);
 const void *(*get_glyph_bitmap)(lv_font_glyph_dsc_t *,lv_draw_buf_t *);
 unsigned line_height,base_line,static_bitmap;const void *dsc;
};
static void lv_draw_buf_init(lv_draw_buf_t *b,unsigned w,unsigned h,unsigned cf,unsigned stride,void *data,unsigned len) {
 assert(cf==8 && len==w*h);b->data=data;b->header.w=w;b->header.h=h;b->header.stride=stride;
}
''' +implementation+r'''
int main(void) {
 assert(aster_flux_font_pixels(16,112,"SDK APP")==12);
 assert(aster_flux_font_pixels(16,128,"上下拨动")==14);
 assert(aster_flux_font_pixels(16,192,"aster")==12);
 assert(aster_flux_font_pixels(20,384,"音乐")==30);
 assert(aster_flux_font(12,"上下拨动") && aster_flux_font(12,"aster"));
 const lv_font_t *f=aster_flux_font(30,"音乐");assert(f && f->static_bitmap);
 assert(aster_flux_font(16,"Wi-Fi 100%") && !aster_flux_font(15,"test"));
 assert(!aster_flux_font(30,"🙂") && !aster_flux_font(16,"\xc3"));
 lv_font_glyph_dsc_t g={0};assert(f->get_glyph_dsc(f,&g,0x97f3,0));
 assert(g.box_w && g.box_h && g.adv_w>0 && g.stride==g.box_w);
 g.resolved_font=f;g.req_raw_bitmap=true;
 const void *raw=f->get_glyph_bitmap(&g,NULL);assert(raw);
 g.req_raw_bitmap=false;
 const lv_draw_buf_t *wrapper=f->get_glyph_bitmap(&g,NULL);
 assert(wrapper->data==raw && wrapper->header.w==g.box_w && wrapper->header.h==g.box_h);
 uint8_t pixels[4096];lv_draw_buf_t out={.data=pixels};
 assert(f->get_glyph_bitmap(&g,&out)==&out && !memcmp(pixels,raw,g.box_w*g.box_h));
 assert(!f->get_glyph_dsc(f,&g,0x1f642,0));
 /* Native small glyphs retain true A8 coverage rather than a 4bpp
  * quantization round trip (which restricts every byte to multiples of 17). */
 f=aster_flux_font(12,"aster");assert(f->get_glyph_dsc(f,&g,'e',0));
 g.resolved_font=f;g.req_raw_bitmap=true;
 const uint8_t *small=f->get_glyph_bitmap(&g,NULL);bool full_a8=false;
 for(unsigned i=0;i<g.box_w*g.box_h;i++) if(small[i]%17) full_a8=true;
 assert(full_a8);
 return 0;
}
''')
