"""Compare the real optimized scaler with independent exact nearest sampling."""
import unittest
from test_waveshare import ROOT, run_c

class ScaleFit(unittest.TestCase):
    def test_exact_pixels_borders_pitch_and_byte_order(self):
        source=(ROOT/'platform/esp32s3/display.c').read_text()
        code=source[source.index('static void scale_fit_box('):source.index('static int scale_fit_send_front(')]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
static uint16_t sys_cpu_to_be16(uint16_t v) {return (v<<8)|(v>>8);}
''' + code + r'''
int main(void) {
 const uint32_t inputs[][2]={{240,320},{320,480},{360,240},{1,1},{513,257},{321,479},{23,17}};
 const uint32_t panels[][2]={{320,480},{240,320},{480,320},{1,1},{99,143},{400,240}};
 for(unsigned a=0;a<sizeof(inputs)/sizeof(inputs[0]);a++)
 for(unsigned b=0;b<sizeof(panels)/sizeof(panels[0]);b++)
 for(int swap=0;swap<2;swap++)
 for(int packed=0;packed<2;packed++) {
  uint32_t bw=inputs[a][0],bh=inputs[a][1],gw=panels[b][0],gh=panels[b][1];
  uint32_t sp=bw+(packed?0:3),dp=gw+(packed?0:5),dw,dh,mx,my;
  unsigned offset=packed?2:1;
  uint16_t *src=malloc((size_t)sp*bh*2);
  size_t n=(size_t)dp*gh;
  uint16_t *guard=malloc((n+4)*2),*out=guard+offset,*ref=malloc(n*2);
  assert(src && guard && ref);
  for(size_t i=0;i<(size_t)sp*bh;i++) src[i]=(uint16_t)((i*313u)^0x84e3u);
  for(size_t i=0;i<n+4;i++) guard[i]=0xdead;
  for(size_t i=0;i<n;i++) ref[i]=0xdead;
  scale_fit_box(bw,bh,gw,gh,&dw,&dh,&mx,&my);
  assert(dw>0 && dh>0 && mx+dw<=gw && my+dh<=gh);
  for(uint32_t y=0;y<gh;y++) for(uint32_t x=0;x<gw;x++) {
   uint16_t v=0;
   if(x>=mx && x<mx+dw && y>=my && y<my+dh) {
    uint32_t sx=(uint32_t)((uint64_t)(x-mx)*bw/dw);
    uint32_t sy=(uint32_t)((uint64_t)(y-my)*bh/dh);
    v=src[(size_t)sy*sp+sx];if(swap) v=sys_cpu_to_be16(v);
   }
   ref[(size_t)y*dp+x]=v;
  }
  scale_fit_blit(out,dp,swap,(const uint8_t *)src,sp,bw,bh,gw,gh,dw,dh,mx,my);
  assert(!memcmp(out,ref,n*2));for(unsigned i=0;i<offset;i++) assert(guard[i]==0xdead);
  for(size_t i=offset+n;i<n+4;i++) assert(guard[i]==0xdead);
  free(src);free(guard);free(ref);
 }
 return 0;
}
''')

    def test_persistent_front_jobs_keep_cache_flush_and_slot_ownership(self):
        source=(ROOT/'platform/esp32s3/display.c').read_text()
        code=source[source.index('static int scale_fit_send_front('):source.index('void aster_plat_display_scale_fit_disarm(')]
        for panel in (0,1):
            with self.subTest(axs_panel=panel):
                run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define IS_ENABLED(x) (x)
#define CONFIG_ASTER_PANEL_AXS15231B '''+str(panel)+r'''
#define CONFIG_ASTER_DISP_ASYNC 1
#define ASTER_DISP_FLIP_SLOT0 1
#define ASTER_OK 0
#define ASTER_EIO -7
struct aster_disp_job {
 const uint8_t *buf;
 struct {size_t buf_size;uint32_t width,height,pitch;} desc;
 uint32_t px,py;bool persistent_ui;
};
static struct {struct aster_disp_job job;} s_slots[3];
static uint8_t s_flip;
static unsigned flushes,records,arms;
static uint16_t frame[320*480];
static void sys_cache_data_flush_range(void *p,size_t n) {
 assert(p==frame && n==sizeof(frame));flushes++;
}
static void glass_note_full(const uint8_t *p,uint32_t w,uint32_t h,uint32_t pitch,bool alias) {
 assert(p==(const uint8_t *)frame && w==320 && h==480 && pitch==320 && alias);records++;
}
static void disp_slot_arm(uint8_t slot) {
 assert(slot==arms+1);struct aster_disp_job *j=&s_slots[slot].job;
 assert(j->buf==(const uint8_t *)frame && j->desc.buf_size==sizeof(frame));
 assert(j->desc.width==320 && j->desc.height==480 && j->desc.pitch==320);
 assert(j->px==0 && j->py==0 && j->persistent_ui==CONFIG_ASTER_PANEL_AXS15231B);arms++;
}
'''+code+r'''
int main(void) {
 assert(scale_fit_send_front(frame,320,480)==0 && s_flip==1);
 assert(scale_fit_send_front(frame,320,480)==0 && s_flip==0);
 assert(records==2 && arms==2 && flushes==2);
 return 0;
}
''')
