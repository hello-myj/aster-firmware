/* Fixed RGB565 scene; compare 17 immediate presents with one existing batch. */
#include <aster_imports.h>
#include <aster/gfx.h>
static aster_handle_t surface;
static aster_gfx_cmd_t commands[17];
static uint32_t frames, total_ms, max_ms, interval_ms, last_ms, errors;
ASTER_APP_EXPORT int aster_app_init(void) {
 aster_surface_desc_t d={.size=sizeof(d),.version=1,.format=ASTER_PIXEL_RGB565};
 int32_t e=aster_surface_create(&d,sizeof(d),&surface);if(e)return e;
 aster_surface_buffer_t b={.size=sizeof(b),.version=1};e=aster_surface_acquire(surface,&b,sizeof(b));if(e)return e;
 commands[0].w[0]=ASTER_GFX_CLEAR;commands[0].w[1]=0x0841;
 for(uint32_t i=0;i<16;i++) {
  uint32_t x=8+(i%8)*(b.width/8), y=20+(i/8)*(b.height/3);
  commands[i+1].w[0]=ASTER_GFX_RECT_F;commands[i+1].w[1]=ASTER_GFX_PACK(x,y);
  commands[i+1].w[2]=ASTER_GFX_PACK(b.width/10,b.height/5);commands[i+1].w[3]=(i*3001+0x1f)&0xffff;
 }
 aster_app_set_framerate(60);aster_logf(ASTER_LOG_INFO,"GFXBENCH mode=%s display=%ux%u",BENCH_NAME,b.width,b.height);return 0;
}
ASTER_APP_EXPORT void aster_app_event(const aster_event_t *ev) {
 if(ev->type!=ASTER_EVENT_FRAME||frames>=128)return;
 uint32_t now=aster_time_now_ms();if(frames)interval_ms+=now-last_ms;last_ms=now;
#if BENCH_BATCH
 int32_t r=aster_gfx_exec(surface,commands,17,0,0,0,0,0,0);if(r!=17)errors++;
#else
 for(uint32_t i=0;i<17;i++){int32_t r=aster_gfx_exec(surface,commands+i,1,0,0,0,0,0,0);if(r!=1)errors++;}
#endif
 uint32_t elapsed=aster_time_now_ms()-now;total_ms+=elapsed;if(elapsed>max_ms)max_ms=elapsed;
 if(++frames==128)aster_logf(ASTER_LOG_INFO,"GFXBENCH DONE mode=%s frames=%u draw_total_ms=%u draw_max_ms=%u interval_total_ms=%u intervals=127 gfx_calls=%u errors=%u",BENCH_NAME,frames,total_ms,max_ms,interval_ms,frames*(BENCH_BATCH?1:17),errors);
}
ASTER_APP_EXPORT void aster_app_stop(void){if(surface)aster_surface_destroy(surface);surface=0;}
