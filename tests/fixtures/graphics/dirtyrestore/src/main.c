/* External diagnostic fill must be replaced by the next unchanged END. */
#include <aster_imports.h>
#include <aster_gfx_engine.h>
static aster_handle_t surface;static aster_ge_t ge;
static aster_gfx_cmd_t ring[4];static uint8_t arena[16];
static uint32_t started,phase;
ASTER_APP_EXPORT int aster_app_init(void) {
 aster_surface_desc_t d={.size=sizeof(d),.version=1,.format=ASTER_PIXEL_RGB565};
 int e=aster_surface_create(&d,sizeof(d),&surface);if(e)return e;
 aster_surface_buffer_t b={.size=sizeof(b),.version=1};e=aster_surface_acquire(surface,&b,sizeof(b));if(e)return e;
 aster_ge_init(&ge,surface,b.width,b.height,ring,4,arena,sizeof(arena));
 e=aster_ge_begin_frame(&ge);if(e)return e;
 aster_ge_clear(&ge,0x001f);e=aster_ge_present(&ge);if(e<0)return e;
 started=aster_time_now_ms();aster_app_set_framerate(60);
 aster_logf(ASTER_LOG_INFO,"DIRTYRESTORE BASE BLUE hold=20s");return 0;
}
ASTER_APP_EXPORT void aster_app_event(const aster_event_t *ev) {
 if(ev->type!=ASTER_EVENT_FRAME||phase||aster_time_now_ms()-started<20000)return;
 int e=aster_ge_begin_frame(&ge);if(!e)e=aster_ge_present(&ge);phase=1;
 aster_logf(e<0?ASTER_LOG_ERROR:ASTER_LOG_INFO,"DIRTYRESTORE END result=%d",e);
}
ASTER_APP_EXPORT void aster_app_stop(void){if(ge.frame_id)aster_ge_cancel_frame(&ge);if(surface)aster_surface_destroy(surface);}
