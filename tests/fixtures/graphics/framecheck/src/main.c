#include <aster_imports.h>
#include <aster_gfx_engine.h>
static aster_handle_t surface;
static aster_ge_t ge;
static aster_gfx_cmd_t ring[3];static uint8_t arena[24];
static uint16_t *pixels;static uint32_t w,h,pitch,started,phase;
static int32_t failed(const char *s,int32_t e){aster_logf(ASTER_LOG_ERROR,"FRAMECHECK FAIL %s result=%d",s,(int)e);return ASTER_ECORRUPT;}
static int unchanged(void){return pixels[0]==0x001f&&pixels[(h-1)*pitch+w-1]==0x001f;}
ASTER_APP_EXPORT int aster_app_init(void){
 aster_surface_desc_t d={.size=sizeof(d),.version=1,.format=ASTER_PIXEL_RGB565};
 int32_t e=aster_surface_create(&d,sizeof(d),&surface);if(e)return e;
 aster_surface_buffer_t b={.size=sizeof(b),.version=1};e=aster_surface_acquire(surface,&b,sizeof(b));if(e)return e;
 pixels=b.ptr;w=b.width;h=b.height;pitch=b.pitch/2;
 aster_ge_init(&ge,surface,w,h,ring,3,arena,24);aster_ge_clear(&ge,0x001f);e=aster_ge_present(&ge);if(e<0)return e;
 e=aster_ge_begin_frame(&ge);if(e)return failed("begin",e);
 uint32_t stale=ge.frame_id;
 aster_ge_clear(&ge,0x07e0);for(int i=0;i<12;i++)aster_ge_rect(&ge,i*8,0,4,4,0xffff);
 if(!unchanged())return failed("private-guest",0);
 if(aster_ge_cancel_frame(&ge))return failed("cancel",0);
 if(!unchanged())return failed("cancel-pixels",0);
 aster_gfx_cmd_t clear={{ASTER_GFX_CLEAR,0xf800}};
 e=aster_gfx_exec(surface,&clear,1,0,0,stale,ASTER_GFX_FRAME_MAGIC,0,0);
 if(e!=ASTER_ESTATE||!unchanged())return failed("stale-batch",e);
 e=aster_ge_begin_frame(&ge);if(e)return failed("begin-error",e);
 aster_ge_clear(&ge,0x07e0);for(int i=0;i<6;i++)aster_ge_rect(&ge,i*8,0,4,4,0xffff);
 aster_ge_text(&ge,0,20,16,0xffff,ASTER_GFX_NOBG,"this text is deliberately too large for the arena");
 e=aster_ge_present(&ge);if(e>=0||!unchanged())return failed("encoder-error",e);
 e=aster_ge_begin_frame(&ge);if(e)return failed("begin-resource",e);
 aster_ge_clear(&ge,0x07e0);for(int i=0;i<6;i++)aster_ge_rect(&ge,i*8,0,4,4,0xffff);
 aster_gfx_cmd_t bad={{ASTER_GFX_BMP,0,0xffffffff}};
 e=aster_gfx_exec(surface,&bad,1,0,0,ge.frame_id,ASTER_GFX_FRAME_MAGIC,0,0);
 if(e!=ASTER_EBADF)return failed("bad-resource",e);
 e=aster_ge_present(&ge);if(e!=ASTER_EBADF||!unchanged())return failed("poison-end",e);
 e=aster_ge_begin_frame(&ge);if(e)return failed("begin-alignment",e);
 uint8_t unaligned[sizeof(clear)+4];uint32_t off=(4u-(uint32_t)(uintptr_t)unaligned%4u)%4u;
 memcpy(unaligned+off+1,&clear,sizeof(clear));
 e=aster_gfx_exec(surface,(const aster_gfx_cmd_t*)(unaligned+off+1),1,0,0,ge.frame_id,ASTER_GFX_FRAME_MAGIC,0,0);
 if(e!=ASTER_EINVAL)return failed("alignment",e);
 e=aster_ge_present(&ge);if(e!=ASTER_EINVAL||!unchanged())return failed("alignment-end",e);
 e=aster_ge_begin_frame(&ge);if(e)return failed("begin-timeout",e);
 aster_ge_clear(&ge,0x07e0);for(int i=0;i<6;i++)aster_ge_rect(&ge,i*8,0,4,4,0xffff);
 started=aster_time_now_ms();aster_app_set_framerate(60);
 aster_logf(ASTER_LOG_INFO,"FRAMECHECK PRIVATE PASS cancel/stale-batch/encoder/bad-resource; blue stays visible");return 0;
}
ASTER_APP_EXPORT void aster_app_event(const aster_event_t *e){
 if(e->type!=ASTER_EVENT_FRAME||phase)return;
 if(aster_time_now_ms()-started<1100)return;
 int32_t r=aster_ge_present(&ge);
 if(r!=ASTER_ETIMEDOUT||!unchanged()){phase=2;failed("timeout",r);return;}
 r=aster_ge_begin_frame(&ge);if(r){phase=2;failed("recovery",r);return;}
 aster_ge_clear(&ge,0x0841);
 aster_ge_text(&ge,8,36,20,0x07e0,ASTER_GFX_NOBG,"FRAME PASS");
 for(int i=0;i<12;i++)aster_ge_rect(&ge,8+i*16,72,8,24,0x07e0);
 r=aster_ge_present(&ge);phase=r<0?2:1;
 if(r<0){failed("commit",r);return;}
 if(pixels[0]!=0x0841){phase=2;failed("guest-commit",0);return;}
 aster_logf(ASTER_LOG_INFO,"FRAMECHECK PASS private/cancel/stale/error/alignment/timeout/recovery/commit");
}
ASTER_APP_EXPORT void aster_app_stop(void){if(ge.frame_id)aster_ge_cancel_frame(&ge);if(surface)aster_surface_destroy(surface);}
