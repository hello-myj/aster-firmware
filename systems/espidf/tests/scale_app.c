/* SPDX-License-Identifier: MIT */
#include <aster/aster.h>
#include <stddef.h>
void *memcpy(void *p,const void *q,size_t n) {unsigned char *d=p;const unsigned char *s=q;while(n--)*d++=*s++;return p;}
void *memset(void *p,int v,size_t n) {unsigned char *d=p;while(n--)*d++=v;return p;}
static aster_handle_t surface;static unsigned events;
int32_t aster_app_init(void)
{
    aster_surface_desc_t desc={sizeof(desc),1,4,2,ASTER_PIXEL_RGB565,ASTER_SURFACE_F_SCALE_FIT};
    int r=aster_surface_create(&desc,sizeof(desc),&surface);if(r)return r;
    aster_surface_buffer_t buffer;r=aster_surface_acquire(surface,&buffer,sizeof(buffer));if(r)return r;
    if(buffer.width!=4 || buffer.height!=2 || buffer.pitch!=8 || buffer.flags!=ASTER_SURFACE_F_SCALE_FIT)return 10;
    uint16_t colors[]={0xf800,0xf800,0x001f,0x001f,0x07e0,0x07e0,0xffff,0xffff};
    memcpy(buffer.ptr,colors,sizeof(colors));return aster_surface_present(surface);
}
void aster_app_start(void) {}
void aster_app_event(const aster_event_t *event)
{
    if(event->type!=ASTER_EVENT_TOUCH)return;
    if(event->u.touch.x!=2 || event->u.touch.y!=1){aster_app_exit(11);return;}
    aster_touch_point_t point;
    if(++events==1) {
        if(event->u.touch.action!=ASTER_TOUCH_DOWN || aster_input_touch((uint32_t)(uintptr_t)&point,1)!=1 || point.x!=2 || point.y!=1)aster_app_exit(12);
    } else aster_app_exit(event->u.touch.action==ASTER_TOUCH_UP && aster_input_touch((uint32_t)(uintptr_t)&point,1)==0?0:13);
}
void aster_app_stop(void) {}
void aster_app_destroy(void) {}
