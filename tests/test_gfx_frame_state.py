"""Actual bounded frame state: private pixels, budgets, timeout and cancel."""
import unittest
from test_waveshare import ROOT,run_c
class FrameState(unittest.TestCase):
 def test_staging_cancel_canvas_and_resource_bounds(self):
  header=str(ROOT/'systems/zephyr/runtime/include/aster_gfx_frame.h')
  code=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "HEADER"
static int live,failed;
static void *alloc(size_t n){if(failed)return NULL;live++;return malloc(n);}
static void release(void *p){live--;free(p);}
int main(void){
 aster_frame_state_t f={0};uint16_t guest[16];for(int i=0;i<16;i++)guest[i]=0x1234;
 assert(aster_frame_start(&f,1,4,4,false,(uint8_t*)guest,4,0,alloc,release)==0);
 assert(live==1&&f.id==1&&f.state==ASTER_FRAME_DRAWING);
 memset(f.pixels,0,32);assert(guest[0]==0x1234);
 assert(aster_frame_start(&f,1,4,4,false,(uint8_t*)guest,4,0,alloc,release)==ASTER_EBUSY);
 aster_gfx_cmd_t clear={{ASTER_GFX_CLEAR,0xffff}};
 assert(aster_frame_batch(&f,1,&clear,1,0,10)==0);
 assert(aster_frame_batch(&f,2,&clear,1,0,10)==ASTER_EBUSY);
 assert(aster_frame_batch(&f,1,&clear,1,0,10)==ASTER_EBUSY);
 aster_frame_abort(&f);assert(guest[0]==0x1234&&f.state==ASTER_FRAME_FREE);
 assert(aster_frame_start(&f,1,4,4,true,NULL,4,20,alloc,release)==0);
 aster_gfx_cmd_t rect={{ASTER_GFX_RECT_F}};
 assert(aster_frame_batch(&f,1,&rect,1,0,30)==ASTER_ESTATE);
 aster_frame_abort(&f);
 assert(aster_frame_start(&f,1,4,4,true,NULL,4,20,alloc,release)==0);
 assert(aster_frame_batch(&f,1,&clear,1,0,30)==0);
 assert(aster_frame_check(&f,1020)==ASTER_ETIMEDOUT);
 aster_frame_abort(&f);
 assert(aster_frame_start(&f,1,4,4,false,(uint8_t*)guest,4,0,alloc,release)==0);
 f.commands=ASTER_GFX_FRAME_MAX_COMMANDS;assert(aster_frame_batch(&f,1,&clear,1,0,1)==ASTER_ENOSPC);
 aster_frame_abort(&f);failed=1;
 assert(aster_frame_start(&f,1,8,8,true,NULL,8,0,alloc,release)==ASTER_ENOMEM);
 assert(live==1&&f.capacity==32&&f.state==ASTER_FRAME_FREE);
 assert(aster_frame_start(&f,1,1000,1000,true,NULL,1000,0,alloc,release)==ASTER_EINVAL);
 assert(f.cache_surface==1);aster_frame_release_cache(&f,release);assert(live==0&&!f.pixels&&!f.capacity&&!f.cache_surface);return 0;
}
'''.replace('HEADER',header)
  # Contract headers are public C types; make them visible to the real helper.
  code=code.replace('#include "'+header+'"','#include "'+str(ROOT/'contract/include/aster/gfx.h')+'"\n#include "'+str(ROOT/'contract/include/aster/error.h')+'"\n#define ASTER_GFX_ENGINE_ABI_H\n#define ASTER_ERROR_H\n'+(ROOT/'systems/zephyr/runtime/include/aster_gfx_frame.h').read_text().replace('#include <aster/gfx.h>','').replace('#include <aster/error.h>',''))
  run_c(code)
