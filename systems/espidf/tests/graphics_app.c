/* SPDX-License-Identifier: MIT */
/* Compiled to real WASM and executed through the IDF native bridges. */
#include <aster/aster.h>
#include <stddef.h>
void *memset(void *p,int value,size_t n) { unsigned char *d=p; while(n--) *d++=(unsigned char)value; return p; }
void *memcpy(void *p,const void *q,size_t n) { unsigned char *d=p; const unsigned char *s=q; while(n--) *d++=*s++; return p; }
static aster_handle_t surface;
static int touches;
static int fail(int n) { aster_app_exit(n); return n; }
#define CHECK(c,n) do { if (!(c)) return fail(n); } while (0)
int32_t aster_app_init(void)
{
    CHECK(aster_system_get_abi_version()==ASTER_ABI_VERSION, 10);
    CHECK(aster_display_count()==1, 11);
    CHECK(aster_system_has_feature(ASTER_FEATURE_DISPLAY), 12);
    CHECK(aster_system_has_feature(ASTER_FEATURE_TOUCH), 13);
    aster_display_info_t info={0};
    CHECK(aster_display_get_info(0,&info,sizeof(info))==0,14);
    CHECK(info.width==480 && info.height==480 && info.app_width==480,15);
    aster_input_info_t input={0};
    CHECK(aster_input_get_info(&input,sizeof(input))==0 && input.max_points==1,16);
    aster_surface_desc_t desc={sizeof(desc),1,0,0,ASTER_PIXEL_NATIVE,0};
    CHECK(aster_surface_create(&desc,sizeof(desc),&surface)==0,17);
    aster_handle_t original=surface, extra;
    CHECK(aster_surface_create(&desc,sizeof(desc),&extra)==ASTER_EBUSY,18);
    aster_surface_buffer_t buffer={0};
    CHECK(aster_surface_acquire(surface,&buffer,sizeof(buffer))==0,19);
    CHECK(buffer.ptr && buffer.width==480 && buffer.pitch==960 && buffer.size==32,20);
    uint16_t *pixels=buffer.ptr;
    for (unsigned i=0;i<480*480;++i) pixels[i]=0x07e0;
    CHECK(aster_surface_present(surface)==0,21);
    uint16_t rect[]={0xf800,0x001f,0xffff,0x1234,0x5678,0};
    CHECK(aster_surface_flush(surface,rect,1,2,2,2,3)==0,22);
    CHECK(aster_surface_flush(surface,rect,479,0,2,2,3)==ASTER_EINVAL,23);
    CHECK(aster_surface_flush(surface,rect,0,0,2,2,1)==ASTER_EINVAL,24);
    CHECK(aster_surface_flush(surface,rect,0,0,2,2,0xffffffffu)==ASTER_EINVAL,25);
    CHECK(aster_surface_flush(surface,(void *)0xfffffff0u,0,0,2,2,2)==ASTER_EINVAL,32);
    CHECK(aster_display_get_info(0,(void *)0xfffffff0u,sizeof(info))==ASTER_EINVAL,33);
    CHECK(aster_surface_present(0)==ASTER_EBADF,26);
#ifdef TEST_RENDER
    aster_gfx_cmd_t commands[2]={{{ASTER_GFX_CLEAR,0x001f,0,0,0,0}},
        {{ASTER_GFX_RECT_F,ASTER_GFX_PACK(10,10),ASTER_GFX_PACK(20,20),0xf800,0,0}}};
    CHECK(aster_gfx_exec(surface,commands,2,0,0,0,0,0,0)==2,50);
    CHECK(pixels[0]==0x001f && pixels[10+10*480]==0xf800,51);
    commands[0].w[1]=0xffff;commands[1].w[0]=8;
    CHECK(aster_gfx_exec(surface,commands,2,0,0,0,0,0,0)==ASTER_EINVAL,52);
    CHECK(pixels[0]==0x001f,53); /* Invalid batch must not partially clear. */
#endif
    CHECK(aster_surface_destroy(surface)==0,27);
    CHECK(aster_surface_acquire(original,&buffer,sizeof(buffer))==ASTER_EBADF,28);
    desc.flags=ASTER_SURFACE_F_SCALE_FIT;
    CHECK(aster_surface_create(&desc,sizeof(desc),&surface)==ASTER_EINVAL,29);
    desc.flags=0;
    CHECK(aster_surface_create(&desc,sizeof(desc),&surface)==0 && surface!=original,30);
    CHECK(aster_surface_present(original)==ASTER_EBADF,31);
    return 0;
}
void aster_app_start(void) {}
void aster_app_event(const aster_event_t *event)
{
    if (event->type!=ASTER_EVENT_TOUCH) return;
    const unsigned actions[]={ASTER_TOUCH_DOWN,ASTER_TOUCH_MOVE,ASTER_TOUCH_UP};
    if (touches>=3 || event->u.touch.action!=actions[touches]) {fail(40);return;}
    unsigned x=touches==0 ? 10 : 30, y=touches==0 ? 20 : 40;
    if (event->u.touch.x!=x || event->u.touch.y!=y) {fail(41);return;}
    aster_touch_point_t point;
    int count=aster_input_touch((uint32_t)&point,1);
    if (count!=(touches<2 ? 1 : 0)) {fail(42);return;}
    if (++touches==3) aster_app_exit(0);
}
void aster_app_stop(void) {}
void aster_app_destroy(void) {}
