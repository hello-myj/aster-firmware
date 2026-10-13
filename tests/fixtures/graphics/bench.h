/* Shared 128-frame accounting for the G0 graphics fixtures (text, image,
 * partial, small-ring variants). A mode includes this FIRST (after defining
 * BENCH_NAME), then implements bench_setup()/bench_draw(); it may use the
 * shared surface/W/H and the errors/gfx_calls counters. The GFXBENCH DONE
 * line keeps the exact field shape tools/graphics_benchmark.py parses
 * (intervals=127 is a literal there, so frame count and parser move together).
 * Not used by the original individual/batched modes (those keep scene.h). */
#include <aster_imports.h>
#include <aster/gfx.h>
static aster_handle_t surface;
static uint32_t W, H, frames, total_ms, max_ms, interval_ms, last_ms, errors, gfx_calls;
static void bench_setup(uint32_t width, uint32_t height); /* mode: load, init */
static void bench_draw(uint32_t frame_no);                /* mode: one frame */
ASTER_APP_EXPORT int aster_app_init(void) {
 aster_surface_desc_t d={.size=sizeof(d),.version=1,.format=ASTER_PIXEL_RGB565};
 int32_t e=aster_surface_create(&d,sizeof(d),&surface);if(e)return e;
 aster_surface_buffer_t b={.size=sizeof(b),.version=1};e=aster_surface_acquire(surface,&b,sizeof(b));if(e)return e;
 W=b.width;H=b.height;bench_setup(W,H);gfx_calls=0;
 aster_app_set_framerate(60);
 aster_logf(ASTER_LOG_INFO,"GFXBENCH mode=%s display=%ux%u",BENCH_NAME,W,H);return 0;
}
ASTER_APP_EXPORT void aster_app_event(const aster_event_t *ev) {
 if(ev->type!=ASTER_EVENT_FRAME||frames>=128)return;
 uint32_t now=aster_time_now_ms();if(frames)interval_ms+=now-last_ms;last_ms=now;
 bench_draw(frames);
 uint32_t elapsed=aster_time_now_ms()-now;total_ms+=elapsed;if(elapsed>max_ms)max_ms=elapsed;
 if(++frames==128)aster_logf(ASTER_LOG_INFO,"GFXBENCH DONE mode=%s frames=%u draw_total_ms=%u draw_max_ms=%u interval_total_ms=%u intervals=127 gfx_calls=%u errors=%u",BENCH_NAME,frames,total_ms,max_ms,interval_ms,gfx_calls,errors);
}
ASTER_APP_EXPORT void aster_app_stop(void){if(surface)aster_surface_destroy(surface);surface=0;}
