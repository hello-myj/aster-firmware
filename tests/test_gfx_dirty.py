"""Real bounded damage algorithm and per-board submission/error behavior."""
import unittest
from test_waveshare import ROOT, run_c


class DirtyRegions(unittest.TestCase):
 def test_bounded_coverage_clipping_and_cheapest_merge(self):
  for cap in (1, 8, 16):
   run_c('#define CONFIG_ASTER_GFX_DIRTY_REGIONS '+str(cap)+r'''
#include <assert.h>
#include "aster_dirty.h"
static bool covered(const aster_dirty_set_t *s,uint32_t x,uint32_t y) {
 for(uint32_t i=0;i<s->count;i++) {aster_dirty_rect_t r=s->rect[i];
  if(x>=r.x&&x-r.x<r.w&&y>=r.y&&y-r.y<r.h) return true;
 }return false;
}
int main(void) {
 aster_dirty_set_t s={0};
 aster_dirty_add(&s,64,64,UINT32_MAX,0,UINT32_MAX,1);assert(!s.count);
 aster_dirty_add(&s,64,64,63,63,UINT32_MAX,UINT32_MAX);
 assert(s.count==1&&s.rect[0].w==1&&s.rect[0].h==1);
 s.count=0;aster_dirty_add(&s,64,64,0,0,2,2);aster_dirty_add(&s,64,64,60,60,2,2);
 if(ASTER_DIRTY_CAP>1) assert(s.count==2);
 assert(covered(&s,0,0)&&covered(&s,61,61));
 s.count=0;aster_dirty_add(&s,64,64,0,0,2,2);aster_dirty_add(&s,64,64,62,62,2,2);
 if(ASTER_DIRTY_CAP>1) assert(!aster_dirty_is_full(&s,64,64));
 s.count=0;aster_dirty_add(&s,64,64,0,0,8,8);aster_dirty_add(&s,64,64,4,4,8,8);
 assert(s.count==1&&s.rect[0].w==12&&s.rect[0].h==12);
 uint8_t expected[64*64]={0};uint32_t seed=123;
 s.count=0;
 for(uint32_t n=0;n<500;n++) {
  seed=seed*1664525u+1013904223u;uint32_t x=seed%64;
  seed=seed*1664525u+1013904223u;uint32_t y=seed%64;
  uint32_t w=1+n%7,h=1+n%5;
  aster_dirty_add(&s,64,64,x,y,w,h);assert(s.count<=ASTER_DIRTY_CAP);
  for(uint32_t j=y;j<64&&j-y<h;j++)for(uint32_t i=x;i<64&&i-x<w;i++)expected[j*64+i]=1;
  for(uint32_t j=0;j<64;j++)for(uint32_t i=0;i<64;i++)if(expected[j*64+i])assert(covered(&s,i,j));
 }
 aster_dirty_add(&s,64,64,0,0,64,64);assert(s.count==1&&s.rect[0].w==64&&s.rect[0].h==64&&aster_dirty_is_full(&s,64,64));
 return 0;
}
''')

 def submission_code(self, axs):
  source=(ROOT/'platform/esp32s3/display.c').read_text()
  body=source[source.index('int aster_plat_display_write_app_regions('):source.index('int aster_plat_display_write_at(const')]
  return '#define CONFIG_ASTER_PANEL_AXS15231B '+str(int(axs))+r'''
#include <assert.h>
#include <string.h>
#include "aster_dirty.h"
#define CONFIG_ASTER_DISP_TIMING 0
#define CONFIG_ASTER_APP_SPACE_DIV 1
#define IS_ENABLED(x) (x)
#define K_FOREVER 0
#define ASTER_OK 0
#define ASTER_EINVAL -1
#define ASTER_ENODEV -2
#define ASTER_ENOMEM -3
#define ASTER_EIO -7
static int disp_lock,driver_lock,display_generation;
static int writes,streams,fail_write,fail_stream,completion_error,rot;
static bool s_damage_gather_only;
static uint64_t s_completed_bytes,s_completed_windows;
static const void *disp_dev=(void *)1;
static uint16_t shadow[8*8];
static void k_mutex_lock(int *p,int wait){(void)wait;++*p;}
static void k_mutex_unlock(int *p){assert(*p>0);--*p;}
static void atomic_inc(int *p){++*p;}
static void sys_cache_data_flush_range(void *p,size_t n){(void)p;(void)n;}
static void panel_geom(uint32_t *w,uint32_t *h){*w=*h=8;}
static int aster_plat_display_count(void){return 1;}
static void aster_plat_display_flush_drain(void){assert(!driver_lock);}
static int aster_plat_display_flush_drain_checked(void){assert(!driver_lock&&!disp_lock);return completion_error;}
static uint8_t *aster_axs15231b_shadow_begin(const void *d,bool all){(void)d;(void)all;assert(disp_lock);assert(!driver_lock);driver_lock++;return (uint8_t*)shadow;}
static int aster_axs15231b_shadow_present_counted(const void *d,uint32_t x,uint32_t y,uint32_t w,uint32_t h,size_t *bytes){
 (void)d;assert(!x&&!y&&w==8&&h==8);assert(driver_lock==1&&!s_damage_gather_only);driver_lock--;streams++;*bytes=fail_stream?0:128;return fail_stream?-1:0;
}
static int aster_plat_display_write_app_at(const uint8_t *src,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch){
 assert(pitch==10);writes++;
 if(fail_write==writes)return ASTER_EIO;
#if CONFIG_ASTER_PANEL_AXS15231B
 assert(s_damage_gather_only&&disp_lock);assert(streams==0);
#endif
 for(uint32_t j=0;j<h;j++)for(uint32_t i=0;i<w;i++) {
  uint32_t dx=x+i,dy=y+j;
  if(rot==90){uint32_t save=dx;dx=dy;dy=7-save;}
  shadow[dy*8+dx]=((const uint16_t*)src)[j*pitch+i];
 }return 0;
}
''' + body

 def test_axs_gathers_once_rotation_pitch_and_failure_unlock(self):
  run_c(self.submission_code(True)+r'''
int main(void){
 uint16_t src[8*10];for(int i=0;i<80;i++)src[i]=i+1;
 aster_dirty_set_t s={.count=2,.rect={{1,1,2,2},{5,5,2,2}}};
 for(int angle=0;angle<=90;angle+=90) {
  writes=streams=0;rot=angle;memset(shadow,0,sizeof(shadow));
  assert(aster_plat_display_write_app_regions((uint8_t*)src,8,8,10,&s)==0);
  assert(writes==2&&streams==1&&!disp_lock&&!driver_lock&&!s_damage_gather_only);
  if(!rot)assert(shadow[1*8+1]==src[1*10+1]&&shadow[5*8+5]==src[5*10+5]);
  else assert(shadow[6*8+1]==src[1*10+1]&&shadow[2*8+5]==src[5*10+5]);
 }
 writes=streams=0;fail_write=2;
 assert(aster_plat_display_write_app_regions((uint8_t*)src,8,8,10,&s)==ASTER_EIO);
 assert(!streams&&!disp_lock&&!driver_lock&&!s_damage_gather_only);
 fail_write=0;fail_stream=1;writes=streams=0;
 assert(aster_plat_display_write_app_regions((uint8_t*)src,8,8,10,&s)==ASTER_EIO);
 assert(streams==1&&!disp_lock&&!driver_lock&&!s_damage_gather_only);
 s.rect[1].w=UINT32_MAX;writes=streams=0;
 assert(aster_plat_display_write_app_regions((uint8_t*)src,8,8,10,&s)==ASTER_EINVAL);
 assert(!writes&&!streams);return 0;
}
''')

 def test_lckfb_keeps_separate_windows_and_completion_error(self):
  # Unused AXS stub data/functions are excluded from the LCKFB build.
  code=self.submission_code(False).replace('static uint64_t s_completed_bytes,s_completed_windows;','').replace('static const void *disp_dev=(void *)1;','')
  code=code.replace('static int disp_lock,driver_lock,display_generation;','static int disp_lock,driver_lock;')
  code=code.replace('static void atomic_inc(int *p){++*p;}','')
  begin=code.index('static int aster_axs15231b_shadow_present_counted(')
  end=code.index('static int aster_plat_display_write_app_at(',begin)
  # Earlier begin stub can be unused; it does not refer to deleted variables.
  code=code[:begin]+code[end:]
  run_c(code+r'''
int main(void){
 uint16_t src[80]={0};aster_dirty_set_t s={.count=2,.rect={{1,1,2,2},{5,5,2,2}}};
 assert(aster_plat_display_write_app_regions((uint8_t*)src,8,8,10,&s)==0);
 assert(writes==2&&!streams&&!s_damage_gather_only&&!fail_stream);
 completion_error=ASTER_EIO;writes=0;
 assert(aster_plat_display_write_app_regions((uint8_t*)src,8,8,10,&s)==ASTER_EIO);
 assert(writes==2&&!disp_lock);return 0;
}
''')
