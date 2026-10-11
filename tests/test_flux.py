"""Exercise FLUX cyclic selection and app-edge return using the actual C helpers."""
import unittest
from test_waveshare import ROOT, run_c

class Flux(unittest.TestCase):
    def test_neighbor_cache_retains_existing_textures(self):
        source=(ROOT/'systems/zephyr/ui/launcher/flux.c').read_text()
        helper=source[source.index('static int cache_slot'):source.index('static void ribbon_order')]
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

    def test_stable_layer_order_does_not_invalidate_stage(self):
        source=(ROOT/'systems/zephyr/ui/launcher/flux.c').read_text()
        cyclic=source[source.index('static int nearest'):source.index('static entry_t *current')]
        order=source[source.index('static void ribbon_order'):source.index('static void redraw(void)\n{')]
        run_c(r'''
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#define RIBBONS 5
typedef struct {int id;} lv_obj_t;
static lv_obj_t objects[6]={{0},{1},{2},{3},{4},{5}};
static lv_obj_t *children[6]={objects,objects+1,objects+2,objects+3,objects+4,objects+5};
static lv_obj_t *stage=objects;
static struct {lv_obj_t *obj;} ribbons[5]={{objects+1},{objects+2},{objects+3},{objects+4},{objects+5}};
static int phase,count=13,invalidations;
static int lv_obj_get_child_count(lv_obj_t *o) {(void)o;return 6;}
static void lv_obj_move_to_index(lv_obj_t *o,int target) {
 int old=0;while(children[old]!=o) old++;
 if(old==target) return;
 invalidations++;
 if(old>target) for(int i=old;i>target;i--) children[i]=children[i-1];
 else for(int i=old;i<target;i++) children[i]=children[i+1];
 children[target]=o;
}
''' +cyclic+order+r'''
static void check(const int index[5]) {
 ribbon_order(index);assert(children[0]==objects);
 int prior=4;
 for(int i=1;i<6;i++) {
  int idx=index[children[i]->id-1];
  int depth=idx<0?4:abs(distance(idx,phase,count))/1000;
  assert(depth<=prior);prior=depth;
 }
 int before=invalidations;
 ribbon_order(index);assert(invalidations==before);
}
int main(void) {
 const int resident[5]={11,12,0,1,2};
 for(phase=-400;phase<500;phase+=10) check(resident);
 const int next[5]={3,12,0,1,2};
 for(phase=500;phase<1500;phase+=10) check(next);
 count=3;phase=0;const int small[5]={0,1,2,-1,-1};check(small);
 count=0;const int empty[5]={-1,-1,-1,-1,-1};check(empty);
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
 /* Rounded interpolation preserves constant colors and is symmetric
  * across each edge: half white/black must not acquire a dark bias. */
 /* Check RGB565 interpolation against a floating-point bilinear
  * definition, including arbitrary four-color inputs and exact endpoints. */
 for(unsigned seed=0;seed<256;seed++) {
  uint16_t pixels[4];
  for(unsigned i=0;i<4;i++) pixels[i]=(uint16_t)((seed*1664525u+i*1013904223u)>>8);
  for(unsigned wx=0;wx<=256;wx+=16) for(unsigned wy=0;wy<=256;wy+=16) {
   double fx=wx/256.0,fy=wy/256.0;
   unsigned expected=0;
   for(unsigned c=0;c<3;c++) {
    unsigned shift=c==0?11:c==1?5:0,mask=c==1?63:31;
    double top=((pixels[0]>>shift)&mask)*(1-fx)+((pixels[1]>>shift)&mask)*fx;
    double bottom=((pixels[2]>>shift)&mask)*(1-fx)+((pixels[3]>>shift)&mask)*fx;
    expected|=(unsigned)(top*(1-fy)+bottom*fy+0.5)<<shift;
   }
   assert(sample565(pixels,wx,wy)==expected);
  }
 }
 const uint16_t constant[4]={0x39e7,0x39e7,0x39e7,0x39e7};
 const uint16_t edge[4]={0,0xffff,0,0xffff},reverse[4]={0xffff,0,0xffff,0};
 assert(sample565(edge,128,56)==0x8410);
 for(unsigned x=0;x<256;x++) for(unsigned y=0;y<256;y++) {
  assert(sample565(constant,x,y)==0x39e7);
  assert(sample565(edge,x,y)==sample565(reverse,256-x,y));
 }
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
 assert(dst[323+20*322+19]==swap16(0x8000));
 assert(dst[323+20*322+20]==swap16(0xf800));
 aster_flux_draw_prepare(src,720);
 memset(dst,0,sizeof(dst));
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
 assert(dst[323+20*322+19]==swap16(0x8000));
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
 /* Fractional text and artwork both retain smooth, unbiased coverage. */
 for(int i=0;i<360*140;i++) src[i]=(i%360)&1?0xffff:0;
 d.rotation=0;d.scale_x=d.scale_y=512;d.pivot=(point){0,0};d.opa=255;
 coords=(lv_area_t){20,10,379,149};memset(dst,0,sizeof(dst));
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==0);
 assert(dst[323+150*322+261]==swap16(0x8410));
 assert(dst[323+150*322+51]==swap16(0x8410));
 d.colorkey=src;
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==1);
 d.colorkey=NULL;
 d.antialias=0;layer.color_format=1;
 assert(aster_flux_draw_image(1,1,src,&coords,720,&clip,&task,&d)==1);
 return 0;
}
''')

    def test_desktop_font_uses_shared_native_size_cache(self):
        s=(ROOT/'systems/zephyr/ui/launcher/flux_font.c').read_text()
        code=s[s.index('unsigned aster_flux_font_pixels'):]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef struct {unsigned px;} lv_font_t;
static lv_font_t requested;
static unsigned warmed;
static const lv_font_t *aster_ui_font(uint8_t px) {requested.px=px;return &requested;}
static void aster_ui_font_warm(uint8_t px,const char *s) {assert(s);warmed=px;}
''' +code+r'''
int main(void) {
 assert(aster_flux_font_pixels(16,112,"SDK APP")==12);
 assert(aster_flux_font_pixels(16,128,"上下拨动")==14);
 assert(aster_flux_font_pixels(20,384,"任意应用名字")==30);
 assert(aster_flux_font(30,"任意应用名字")==&requested && requested.px==30 && warmed==30);
 return 0;
}
''')
