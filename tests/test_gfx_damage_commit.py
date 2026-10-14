"""Actual transaction END: retain damage only while the panel still matches."""
import unittest
from test_waveshare import ROOT, run_c


class DamageCommit(unittest.TestCase):
 def test_first_frame_overlay_failure_canvas_and_guest_commit(self):
  native=(ROOT/'systems/zephyr/runtime/aster_native.c').read_text()
  body=native[native.index('static int32_t n_gfx_frame_control('):native.index('static int32_t n_gfx_exec(')]
  run_c(r'''
#include <assert.h>
#include <stdlib.h>
#include "HEADER"
typedef void *wasm_exec_env_t;
typedef struct {uint32_t w,h,pitch,app_offset;bool canvas,scale_fit;} aster_surface_t;
typedef struct {aster_frame_state_t gfx_frame;bool paused;void *module_inst;uint32_t gfx_published_surface;} aster_app_ctx_t;
static aster_app_ctx_t ctx;
static aster_gfx_frame_control_t control;
static uint16_t guest[4*6],shown[4*4],canvas[4*4];
static uint32_t generation;
static bool aster_font_sd_repaint_pending(void){return false;}
static uint32_t aster_font_sd_generation(void){return 37;}
static int full,partial,failure,completion_error,snapshots;
static aster_app_ctx_t *ctx_of(wasm_exec_env_t env){(void)env;return &ctx;}
static void *buf(wasm_exec_env_t env,uint32_t off,uint32_t len){(void)env;(void)off;assert(len==sizeof(control));return &control;}
static void *wasm_runtime_addr_app_to_native(void *inst,uint32_t off){(void)inst;(void)off;return guest;}
static bool glass_is_frozen(void){return false;}
static uint64_t k_uptime_get(void){return 0;}
static void *aster_psram_alloc(size_t n){return malloc(n);}
static void aster_psram_free(void *p){free(p);}
static void aster_plat_display_flush_drain(void){}
static int aster_plat_display_flush_drain_checked(void){return completion_error;}
static uint32_t aster_plat_display_generation(void){return generation;}
static void aster_plat_display_scale_fit_disarm(void){}
static int aster_plat_display_app_present(const uint8_t *p,uint32_t w,uint32_t h,uint32_t pitch){
 assert(w==4&&h==4&&pitch==4);generation++;full++;
 memcpy(shown,p,32);return failure;
}
static int present_scale_fit(aster_surface_t *s,uint8_t *p){return aster_plat_display_app_present(p,s->w,s->h,s->w);}
static uint8_t *aster_plat_display_canvas_back(void){return (uint8_t*)canvas;}
static int aster_plat_display_canvas_present(void){generation++;full++;return failure;}
static int aster_plat_display_write_app_regions(const uint8_t *p,uint32_t w,uint32_t h,uint32_t pitch,const aster_dirty_set_t *s){
 (void)w;(void)h;
 if(!s->count)return 0;
 generation++;partial++;
 for(uint32_t n=0;n<s->count;n++){aster_dirty_rect_t r=s->rect[n];
  for(uint32_t y=r.y;y<r.y+r.h;y++)for(uint32_t x=r.x;x<r.x+r.w;x++)shown[y*4+x]=((const uint16_t*)p)[y*pitch+x];
 }return failure;
}
static void aster_gfx_note_committed_frame(const uint8_t *p,uint32_t w,uint32_t h,uint32_t pitch,bool be){
 (void)p;(void)w;(void)h;(void)pitch;(void)be;snapshots++;
}
'''.replace('HEADER',str(ROOT/'systems/zephyr/runtime/include/aster_gfx_frame.h'))+body+r'''
static int request(aster_surface_t *s,uint32_t action){
 control=(aster_gfx_frame_control_t){sizeof(control),1,action,(action==ASTER_GFX_FRAME_BEGIN||action==ASTER_GFX_FRAME_QUERY)?0:ctx.gfx_frame.id,0,0};
 return n_gfx_frame_control(0,1,s,4,sizeof(control));
}
int main(void){
 aster_surface_t s={4,4,6,1,false,false};
 assert(request(&s,ASTER_GFX_FRAME_QUERY)==0&&control.frame_id==37);
 assert((control.flags&7)==7);
 for(int i=0;i<24;i++)guest[i]=0x1234;
 assert(request(&s,ASTER_GFX_FRAME_BEGIN)==0);
 assert(request(&s,ASTER_GFX_FRAME_END)==0&&full==1&&!partial);
 /* Two private changes, retained physical frame. */
 assert(request(&s,ASTER_GFX_FRAME_BEGIN)==0);
 ((uint16_t*)ctx.gfx_frame.pixels)[0]=0xf800;((uint16_t*)ctx.gfx_frame.pixels)[15]=0x07e0;
 ctx.gfx_frame.dirty=(aster_dirty_set_t){.count=2,.rect={{0,0,1,1},{3,3,1,1}}};
 assert(request(&s,ASTER_GFX_FRAME_END)==0&&full==1&&partial==1);
 assert(shown[0]==0xf800&&shown[15]==0x07e0&&guest[0]==0xf800&&guest[3*6+3]==0x07e0);
 assert(guest[4]==0x1234&&guest[5]==0x1234); /* Guest row padding. */
 assert(request(&s,ASTER_GFX_FRAME_BEGIN)==0);assert(request(&s,ASTER_GFX_FRAME_END)==0);
 assert(full==1&&partial==1); /* No damage, no physical write. */
 generation++;memset(shown,0,sizeof(shown)); /* Overlay/other panel writer. */
 assert(request(&s,ASTER_GFX_FRAME_BEGIN)==0);assert(request(&s,ASTER_GFX_FRAME_END)==0&&full==2);
 assert(shown[0]==guest[0]&&shown[15]==guest[3*6+3]);
 assert(request(&s,ASTER_GFX_FRAME_BEGIN)==0);
 ((uint16_t*)ctx.gfx_frame.pixels)[1]=0x001f;aster_dirty_add(&ctx.gfx_frame.dirty,4,4,1,0,1,1);
 failure=ASTER_EIO;assert(request(&s,ASTER_GFX_FRAME_END)==ASTER_EIO);
 assert(guest[1]==0x1234&&ctx.gfx_frame.published_surface==0);failure=0;
 assert(request(&s,ASTER_GFX_FRAME_BEGIN)==0);assert(request(&s,ASTER_GFX_FRAME_END)==0&&full==3);
 assert(shown[1]==0x1234); /* Full restore after a partial physical failure. */
 s.canvas=true;s.pitch=4;
 assert(request(&s,ASTER_GFX_FRAME_BEGIN)==0);ctx.gfx_frame.initialized=true;
 ((uint16_t*)ctx.gfx_frame.pixels)[0]=0xf800;
 assert(request(&s,ASTER_GFX_FRAME_END)==0&&full==4&&((uint8_t*)canvas)[0]==0xf8&&((uint8_t*)canvas)[1]==0);
 aster_frame_release_cache(&ctx.gfx_frame,aster_psram_free);assert(snapshots==6);
 return 0;
}
''')
