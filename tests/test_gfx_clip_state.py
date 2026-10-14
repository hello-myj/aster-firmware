"""Frame clips are bounded, private state and reset on every BEGIN."""
import unittest
from test_waveshare import ROOT,run_c
class FrameClip(unittest.TestCase):
 def test_clip_bounds_stale_tokens_deadline_and_canvas_initialization(self):
  run_c(r'''
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <assert.h>
#include "HEADER"
int main(void){
 aster_frame_state_t f={0};uint8_t guest[32]={0};
 assert(!aster_frame_start(&f,1,4,4,false,guest,4,0,malloc,free));
 aster_gfx_frame_clip_t c={sizeof(c),1,ASTER_GFX_FRAME_SET_CLIP,f.id,1,1,2,2};
 assert(!aster_frame_set_clip(&f,1,&c,1)&&f.clip_x==1&&f.clip_w==2);
 c.x=UINT32_MAX;assert(aster_frame_set_clip(&f,1,&c,2)==ASTER_EINVAL&&f.error==ASTER_EINVAL);
 aster_frame_abort(&f);assert(!aster_frame_start(&f,1,4,4,false,guest,4,3,malloc,free));assert(f.clip_x==0&&f.clip_w==4);
 c.x=0;assert(aster_frame_set_clip(&f,1,&c,4)==ASTER_ESTATE);aster_frame_abort(&f);
 assert(!aster_frame_start(&f,1,4,4,true,NULL,4,5,malloc,free));c.frame_id=f.id;
 assert(aster_frame_set_clip(&f,1,&c,6)==ASTER_ESTATE);aster_frame_abort(&f);
 assert(!aster_frame_start(&f,1,4,4,true,NULL,4,7,malloc,free));c.frame_id=f.id;c.x=c.y=0;c.w=c.h=4;assert(!aster_frame_set_clip(&f,1,&c,8));
 assert(aster_frame_set_clip(&f,1,&c,1007)==ASTER_ETIMEDOUT);aster_frame_release_cache(&f,free);return 0;
}
'''.replace('HEADER',str(ROOT/'systems/zephyr/runtime/include/aster_gfx_frame.h')))
