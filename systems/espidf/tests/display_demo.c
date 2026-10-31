/* SPDX-License-Identifier: MIT */
/* Raw WASM board demo: checkerboard plus a white marker at the last touch. */
#include <aster/aster.h>
#include <stddef.h>
void *memset(void *p,int v,size_t n) { unsigned char *d=p; while(n--) *d++=(unsigned char)v; return p; }
void *memcpy(void *p,const void *q,size_t n) { unsigned char *d=p; const unsigned char *s=q; while(n--) *d++=*s++; return p; }
static aster_handle_t surface;
static unsigned tx,ty,pressed,dirty=1;
static aster_surface_buffer_t buffer;
int32_t aster_app_init(void)
{
    if (aster_system_get_abi_version()!=ASTER_ABI_VERSION) return ASTER_EABI;
    if (aster_display_count()<1) return ASTER_ENODEV;
    aster_surface_desc_t desc={sizeof(desc),1,0,0,ASTER_PIXEL_RGB565,0};
    int result=aster_surface_create(&desc,sizeof(desc),&surface);
    if (result) return result;
    aster_app_set_framerate(30);
    return 0;
}
void aster_app_start(void) {}
void aster_app_event(const aster_event_t *event)
{
    if (event->type==ASTER_EVENT_TOUCH) {
        tx=event->u.touch.x; ty=event->u.touch.y;
        pressed=event->u.touch.action!=ASTER_TOUCH_UP;
        dirty=1;
    }
    if (event->type!=ASTER_EVENT_FRAME || !dirty) return;
    if (aster_surface_acquire(surface,&buffer,sizeof(buffer))) {aster_app_exit(1);return;}
    for (unsigned y=0;y<buffer.height;++y) {
        uint16_t *row=(uint16_t *)((uint8_t *)buffer.ptr+y*buffer.pitch);
        for (unsigned x=0;x<buffer.width;++x) {
            int dx=(int)x-(int)tx,dy=(int)y-(int)ty;
            row[x]=pressed && dx*dx+dy*dy<225 ? 0xffff :
                   ((x/40+y/40)&1) ? 0xfd20 : 0x001f;
        }
    }
    if (aster_surface_present(surface)) aster_app_exit(2);
    dirty=0;
}
void aster_app_stop(void) {}
void aster_app_destroy(void) {aster_surface_destroy(surface);}
