/* SPDX-License-Identifier: MIT */
#include <aster/aster.h>
#include <aster/gfx_rgb888.h>
static aster_handle_t surface;
static aster_surface_buffer_t buffer;
static void check(int ok,int n){if(!ok)aster_app_exit(n);}
static int batch(aster_gfx_rgb888_cmd_t *c,unsigned count)
{return aster_gfx_exec(surface,(const aster_gfx_cmd_t *)c,ASTER_GFX_RGB888_BATCH,0,0,count,1,0,0);}
int32_t aster_app_init(void)
{
    aster_display_info_t info={0};check(!aster_display_get_info(0,&info,sizeof(info)) && info.native_format==ASTER_PIXEL_BGR888,70);
    aster_surface_desc_t desc={sizeof(desc),1,64,64,ASTER_PIXEL_NATIVE,0};check(!aster_surface_create(&desc,sizeof(desc),&surface),71);
    check(!aster_surface_acquire(surface,&buffer,sizeof(buffer)) && buffer.pitch==192 && buffer.format==ASTER_PIXEL_BGR888,72);
    aster_gfx_frame_control_t q={sizeof(q),1,ASTER_GFX_FRAME_QUERY,0,0,0};
    check(!aster_gfx_exec(surface,0,ASTER_GFX_FRAME_CONTROL,(uint8_t *)&q,sizeof(q),ASTER_GFX_FRAME_MAGIC,1,0,0) && q.flags==ASTER_GFX_RGB888_SUPPORTED,73);
    aster_gfx_rgb888_cmd_t clear={.w={ASTER_RGB888_CLEAR,0,0,0x070503,0,0,0,255}};
    check(batch(&clear,1)==1,74);uint8_t *pixels=buffer.ptr;
    check(pixels[0]==3 && pixels[1]==5 && pixels[2]==7,75);
    aster_gfx_rgb888_cmd_t invalid[2]={clear,clear};invalid[0].w[3]=0xffffff;invalid[1].w[0]=99;
    check(batch(invalid,2)==ASTER_EINVAL && pixels[0]==3 && pixels[2]==7,76);
    aster_gfx_rgb888_cmd_t grad={.w={ASTER_RGB888_GRAD,0,ASTER_GFX_PACK(64,4),0,0xff0000,0,1,255}};
    check(batch(&grad,1)==1,77);
    unsigned distinct=1,last=pixels[2]>>2;
    for(unsigned x=1;x<64;x++){unsigned v=pixels[x*3+2]>>2;if(v!=last){distinct++;last=v;}}
    check(distinct>50,78); /* A 565 intermediate would cap this at 32. */
    check(aster_surface_flush(surface,(void *)0xfffffff0u,0,0,1,1,1)==ASTER_EINVAL,79);
    check(!aster_surface_destroy(surface),80);
    desc.format=ASTER_PIXEL_RGB888;check(!aster_surface_create(&desc,sizeof(desc),&surface),81);
    check(!aster_surface_acquire(surface,&buffer,sizeof(buffer)),82);check(batch(&clear,1)==1,83);
    pixels=buffer.ptr;check(pixels[0]==7 && pixels[1]==5 && pixels[2]==3,84);
    /* Pitched RGB-order source is converted without losing low channel bits. */
    uint8_t patch[]={0x17,0x29,0x3b,0,0,0,0x41,0x53,0x65};
    check(!aster_surface_flush(surface,patch,2,3,1,2,2),85);
    check(aster_surface_flush(surface,patch,63,0,2,1,2)==ASTER_EINVAL,86);
    return 0;
}
void aster_app_start(void){aster_app_exit(0);}
void aster_app_event(const aster_event_t *e){(void)e;}
void aster_app_stop(void){}
void aster_app_destroy(void){}
