#include <aster/aster.h>
#include "aster_gfx_engine.h"
#include "aster_printf.h"
static aster_handle_t surf;
static aster_ge_t ge;
static aster_gfx_cmd_t ring[32];static uint8_t arena[1024];
static uint32_t images[2],iw[2],ih[2],frame,errors,W,H;
static uint64_t total,cold;
static const char *paths[]={"/assets/assets/pixel.bmp","/assets/assets/photo.bmp"};
int aster_app_init(void){
 aster_surface_desc_t d={.size=sizeof(d),.version=1,.format=ASTER_PIXEL_RGB565};
 int r=aster_surface_create(&d,sizeof(d),&surf);if(r<0)return r;
 aster_surface_buffer_t b={.size=sizeof(b),.version=1};r=aster_surface_acquire(surf,&b,sizeof(b));if(r<0)return r;
 W=b.width;H=b.height;aster_ge_init(&ge,surf,W,H,ring,32,arena,sizeof(arena));
 for(int i=0;i<2;i++){r=aster_gfx_bmp_load(paths[i],strlen(paths[i]),32,32,&images[i],&iw[i],&ih[i]);if(r<0)return r;}
 aster_app_set_framerate(20);aster_logf(ASTER_LOG_INFO,"G8QUALITY INIT %ux%u",W,H);return 0;
}
static void text(int x,int baseline,int px,uint16_t fg,uint32_t bg,const char *s){int r=aster_ge_text(&ge,x,baseline,px,fg,bg,s);if(r<0)errors++;}
void aster_app_event(const aster_event_t *e){
 if(e->type!=ASTER_EVENT_FRAME||frame>=13)return;
 uint64_t begin=aster_time_now_ms();
 if(aster_ge_begin_frame(&ge)<0)return;
 aster_ge_clear(&ge,0x1082);
 text(12,24,18,0xffff,ASTER_GFX_NOBG,"G8 原生文字与图片");
 text(12,47,12,0xffff,ASTER_GFX_NOBG,"12px 小字：清晰边缘 012345");
 text(12,72,14,0xffff,ASTER_GFX_NOBG,"14px 小字：音乐设置 ABC");
 text(12,100,16,0xffff,ASTER_GFX_NOBG,"16px 中文：触摸与返回");
 text(12,124,12,0x8410,0x8410,"相同前景背景不应产生亮边");
 for(int i=0;i<2;i++){
  aster_ge_bmp(&ge,images[i],iw[i],ih[i],16+i*100,144,256,255);
  aster_ge_bmp(&ge,images[i],iw[i],ih[i],58+i*100,144,256,128);
 }
 text(12,205,12,0xffff,ASTER_GFX_NOBG,"原尺寸 / 半透明  像素 / 平滑");
 aster_ge_rect(&ge,10,219,W-20,32,0xf800);
 text(14,241,14,0xffff,0x0841,"固定背景：不混入旧红色");
 text(12,H-20,12,0xbdf7,ASTER_GFX_NOBG,"原字号 · 保留灰阶抗锯齿");
 int r=aster_ge_present(&ge);if(r<0){errors++;aster_ge_cancel_frame(&ge);return;}
 uint64_t elapsed=aster_time_now_ms()-begin;
 if(!frame)cold=elapsed;else total+=elapsed;
 if(++frame==13)aster_logf(ASTER_LOG_INFO,"G8QUALITY DONE coldms=%u warmtotal=%u samples=12 errors=%u",(unsigned)cold,(unsigned)total,errors);
}
void aster_app_resume(void){frame=0;total=0;}
void aster_app_destroy(void){for(int i=0;i<2;i++)if(images[i])aster_gfx_bmp_free(images[i]);if(surf)aster_surface_destroy(surf);}
