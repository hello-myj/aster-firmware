/* Device regression: quotas, stale handles and validation before raster. */
#include <aster_imports.h>
#include <aster_gfx_engine.h>
static aster_handle_t surface;
static uint32_t ids[6],width,height,image_w,image_h;
static aster_ge_t ge;
static aster_gfx_cmd_t ring[16];
static uint8_t arena[256];
static int32_t fail(const char *stage,int32_t actual) {
 aster_logf(ASTER_LOG_ERROR,"RESOURCE_CHECK FAIL %s result=%d",stage,(int)actual);
 return ASTER_ECORRUPT;
}
ASTER_APP_EXPORT int aster_app_init(void) {
 aster_surface_desc_t d={.size=sizeof(d),.version=1,.format=ASTER_PIXEL_RGB565};
 int32_t e=aster_surface_create(&d,sizeof(d),&surface);if(e)return e;
 aster_surface_buffer_t b={.size=sizeof(b),.version=1};
 e=aster_surface_acquire(surface,&b,sizeof(b));if(e)return e;
 width=b.width;height=b.height;
 for(uint32_t i=0;i<6;i++) {
  e=aster_gfx_bmp_load("/assets/assets/opaque.bmp",25,16,16,&ids[i],&image_w,&image_h);
  if(e)return fail("load-six",e);
 }
 uint32_t replacement=0,w=0,h=0;
 e=aster_gfx_bmp_load("/assets/assets/opaque.bmp",25,16,16,&replacement,&w,&h);
 if(e!=ASTER_ENOMEM)return fail("quota",e);
 uint32_t stale=ids[0];e=aster_gfx_bmp_free(stale);if(e)return fail("free",e);
 e=aster_gfx_bmp_load("/assets/assets/opaque.bmp",25,16,16,&replacement,&w,&h);
 if(e||replacement==stale)return fail("reuse",e);
 ids[0]=replacement;
 e=aster_gfx_bmp_free(stale);if(e!=ASTER_EBADF)return fail("stale-free",e);
 uint16_t *pixels=(uint16_t*)b.ptr;uint32_t pitch=b.pitch/2;
 for(uint32_t y=0;y<height;y++)for(uint32_t x=0;x<width;x++)pixels[y*pitch+x]=0x7bef;
 aster_gfx_cmd_t commands[2]={{{ASTER_GFX_CLEAR,0xffff}},{{ASTER_GFX_BMP,0,stale}}};
 e=aster_gfx_exec(surface,commands,2,0,0,0,0,width,height);
 if(e!=ASTER_EBADF||pixels[0]!=0x7bef||pixels[(height-1)*pitch+width-1]!=0x7bef)return fail("stale-draw",e);
 e=aster_gfx_exec(surface,commands,1,0,0,width,0,1,1);
 if(e!=ASTER_EINVAL||pixels[0]!=0x7bef)return fail("rect-preflight",e);
 aster_ge_init(&ge,surface,width,height,ring,16,arena,sizeof(arena));
 aster_app_set_framerate(15);
 aster_logf(ASTER_LOG_INFO,"RESOURCE_CHECK PASS quota/stale-free/stale-draw/rect-preflight");
 return 0;
}
ASTER_APP_EXPORT void aster_app_event(const aster_event_t *event) {
 if(event->type!=ASTER_EVENT_FRAME)return;
 aster_ge_clear(&ge,0x0841);
 aster_ge_text(&ge,8,36,20,0x07e0,ASTER_GFX_NOBG,"GFX RESOURCE PASS");
 aster_ge_text(&ge,8,68,16,0xffff,ASTER_GFX_NOBG,"资源句柄与裁剪验证");
 for(uint32_t i=0;i<6;i++)aster_ge_bmp(&ge,ids[i],image_w,image_h,8+(int32_t)i*32,100,256,255);
 int32_t e=aster_ge_present(&ge);
 if(e<0)aster_logf(ASTER_LOG_ERROR,"RESOURCE_CHECK render=%d",(int)e);
}
ASTER_APP_EXPORT void aster_app_stop(void) {
 for(uint32_t i=0;i<6;i++){if(ids[i])aster_gfx_bmp_free(ids[i]);ids[i]=0;}
 if(surface)aster_surface_destroy(surface);surface=0;
}
