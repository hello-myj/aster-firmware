"""Ordinary band/partial writes must retain truthful snapshots without a prior grab."""
import unittest
from test_waveshare import ROOT, run_c

class GlassPartial(unittest.TestCase):
 def test_owned_record_bands_alias_failure_and_reuse(self):
  source=(ROOT/'platform/esp32s3/display.c').read_text()
  code=source[source.index('static const uint8_t *s_glass;'):source.index('#else /* !CONFIG_ASTER_SCREENSHOT */')]+ '\n#endif\n'
  run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_ASTER_SCREENSHOT 1
#define CONFIG_ASTER_PANEL_AXS15231B axs
#define IS_ENABLED(x) (x)
#define SMH_REG_ATTR_EXTERNAL 1
#define __ASSERT(x,...) assert(x)
#define LOG_INF(...)
static int axs,allocs,frees,live;static bool fail;
static void *shared_multi_heap_aligned_alloc(int attr,size_t align,size_t n){assert(attr==1&&align==32);if(fail)return NULL;void*p=malloc(n);assert(p);allocs++;live++;return p;}
static void shared_multi_heap_free(void*p){if(p){frees++;live--;free(p);}}
static void panel_geom(uint32_t*w,uint32_t*h){*w=4;*h=4;}
'''+code+r'''
int main(void){
 uint16_t full[16],patch[4]={101,102,103,104},expected[16];
 for(unsigned i=0;i<16;i++)full[i]=expected[i]=i+1;
 /* Fresh ordinary full present is gathered into bands; no old alias/grab. */
 glass_prepare_write(4,4,true);assert(live==1&&allocs==1&&!s_glass_ephemeral);
 glass_track((uint8_t*)full,0,0,4,2,4,false);
 glass_track((uint8_t*)(full+8),0,2,4,2,4,false);
 assert(s_glass==s_glass_buf&&!memcmp(s_glass,full,sizeof(full)));
 glass_prepare_write(4,4,false);glass_track((uint8_t*)patch,1,1,2,2,2,false);
 expected[5]=101;expected[6]=102;expected[9]=103;expected[10]=104;
 assert(!memcmp(s_glass,expected,sizeof(expected))&&allocs==1);
 /* Repeated full and partial writes reuse bounded storage. */
 for(int i=0;i<100;i++){glass_prepare_write(4,4,true);glass_track((uint8_t*)full,0,0,4,4,4,true);}
 assert(allocs==1&&live==1&&!memcmp(s_glass,full,sizeof(full)));
 /* Flip/UI alias stays zero-copy, materialized only before a partial. */
 glass_note_full((uint8_t*)full,4,4,4,true);assert(s_glass==(uint8_t*)full);
 glass_prepare_write(4,4,false);glass_track((uint8_t*)patch,1,1,2,2,2,false);
 assert(s_glass==s_glass_buf&&!memcmp(s_glass,expected,sizeof(expected)));
 assert(!memcmp(full,(uint16_t[]){1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16},sizeof(full)));
 /* Unknown partial frame remains unavailable, never fabricated as black. */
 glass_forget();glass_prepare_write(4,4,false);assert(!s_glass);
 /* Allocation failure leaves drawing independent and snapshots unavailable. */
 shared_multi_heap_free(s_glass_buf);s_glass_buf=NULL;s_glass_buf_bytes=0;
 fail=true;glass_note_full((uint8_t*)full,4,4,4,true);glass_prepare_write(4,4,false);
 assert(!s_glass&&live==0);glass_prepare_write(4,4,true);assert(!s_glass);
 fail=false;glass_prepare_write(4,4,true);glass_track((uint8_t*)full,0,0,4,4,4,true);
 /* A grab's temporary buffer becomes persistent when live writes require it. */
 s_glass_ephemeral=true;glass_prepare_write(4,4,false);assert(!s_glass_ephemeral);
 /* AXS continues to use its own driver framebuffer without another allocation. */
 int before=allocs;axs=1;glass_prepare_write(8,8,true);assert(allocs==before);
 axs=0;glass_prepare_write(8,8,true);assert(allocs==before+1&&live==1&&s_glass_buf_bytes==128);
 shared_multi_heap_free(s_glass_buf);assert(!live);
 return 0;
}
''')
 def test_prepare_precedes_both_wire_paths(self):
  source=(ROOT/'platform/esp32s3/display.c').read_text()
  write=source[source.index('int aster_plat_display_write_at_ex('):]
  self.assertLess(write.index('glass_prepare_write(pw, ph, whole_panel)'),write.index('if (be_pixels_swapped'))
