#include <aster_imports.h>
#include <aster_gfx_engine.h>
static aster_handle_t surface;
static aster_ge_t ge;
static aster_gfx_cmd_t ring[3];static uint8_t arena[24];
static uint32_t started,phase;
ASTER_APP_EXPORT int aster_app_init(void){
 aster_surface_desc_t d={.size=sizeof(d),.version=1,.format=ASTER_PIXEL_RGB565,.flags=ASTER_SURFACE_F_PANEL_CANVAS};
 int32_t r=aster_surface_create(&d,sizeof(d),&surface);if(r)return r;
 aster_surface_buffer_t b={.size=sizeof(b),.version=1};r=aster_surface_acquire(surface,&b,sizeof(b));if(r)return r;
 if(!(b.flags&ASTER_SURFACE_F_PANEL_CANVAS)){aster_logf(ASTER_LOG_ERROR,"FRAMECANVAS unsupported");return ASTER_ENOTSUP;}
 aster_ge_init(&ge,surface,b.width,b.height,ring,3,arena,24);
 aster_ge_clear(&ge,0x001f);r=aster_ge_present(&ge);if(r<0)return r;
 r=aster_ge_begin_frame(&ge);if(r)return r;
 aster_ge_rect(&ge,0,0,4,4,0xf800);r=aster_ge_present(&ge);if(r!=ASTER_ESTATE)return ASTER_ECORRUPT;
 r=aster_ge_begin_frame(&ge);if(r)return r;
 aster_ge_clear(&ge,0xf800);for(int i=0;i<12;i++)aster_ge_rect(&ge,i*8,0,4,4,0xffff);
 r=aster_ge_cancel_frame(&ge);if(r)return r;
 started=aster_time_now_ms();aster_app_set_framerate(60);
 aster_logf(ASTER_LOG_INFO,"FRAMECANVAS PRIVATE PASS granted=1 display=%ux%u; blue remains for 8s",b.width,b.height);return 0;
}
ASTER_APP_EXPORT void aster_app_event(const aster_event_t *e){
 if(e->type!=ASTER_EVENT_FRAME||phase||aster_time_now_ms()-started<8000)return;
 int32_t r=aster_ge_begin_frame(&ge);if(r){phase=2;aster_logf(ASTER_LOG_ERROR,"FRAMECANVAS FAIL begin=%d",r);return;}
 aster_ge_clear(&ge,0x07e0);for(int i=0;i<12;i++)aster_ge_rect(&ge,8+i*16,72,8,24,0xffff);
 aster_ge_text(&ge,8,36,20,0xffff,ASTER_GFX_NOBG,"CANVAS PASS");
 r=aster_ge_present(&ge);phase=r<0?2:1;
 aster_logf(r<0?ASTER_LOG_ERROR:ASTER_LOG_INFO,"FRAMECANVAS %s result=%d",r<0?"FAIL":"PASS",r);
}
ASTER_APP_EXPORT void aster_app_stop(void){if(ge.frame_id)aster_ge_cancel_frame(&ge);if(surface)aster_surface_destroy(surface);}
