"""Private frame drawing must not replace the committed screenshot source."""
import unittest
from test_waveshare import ROOT,run_c
class FrameSnapshot(unittest.TestCase):
 def test_unpublished_raster_restores_all_snapshot_fields(self):
  source=(ROOT/'systems/zephyr/ui/ui_gfx.c').read_text()
  code=source[source.index('int aster_gfx_exec_unpublished_into('):]
  header=str(ROOT/'contract/include/aster/gfx.h')
  run_c(r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "HEADER"
static const uint8_t *s_last_buf;
static uint32_t s_last_w,s_last_h,s_last_pitch;
static bool s_last_be;
static int aster_gfx_exec_into(uint8_t *pixels,uint32_t w,uint32_t h,uint32_t pitch,const aster_gfx_cmd_t *c,uint32_t n,const uint8_t *a,uint32_t len,uint32_t *dx,uint32_t *dy,uint32_t *dw,uint32_t *dh,bool be){
 (void)c;(void)n;(void)a;(void)len;(void)dx;(void)dy;(void)dw;(void)dh;
 s_last_buf=pixels;s_last_w=w;s_last_h=h;s_last_pitch=pitch;s_last_be=be;pixels[0]=42;return 1;
}
'''.replace('HEADER',header)+code+r'''
int main(void){
 uint8_t committed[32]={7},draft[32]={0};
 aster_gfx_note_committed_frame(committed,4,4,4,true);
 assert(aster_gfx_exec_unpublished_into(draft,2,3,2,0,0,0,0,0,0,0,0)==1);
 assert(draft[0]==42&&committed[0]==7&&s_last_buf==committed);
 assert(s_last_w==4&&s_last_h==4&&s_last_pitch==4&&s_last_be);
 aster_gfx_note_committed_frame(0,0,0,0,false);
 aster_gfx_exec_unpublished_into(draft,2,3,2,0,0,0,0,0,0,0,0);
 assert(!s_last_buf&&!s_last_w&&!s_last_h&&!s_last_pitch&&!s_last_be);return 0;
}
''')
