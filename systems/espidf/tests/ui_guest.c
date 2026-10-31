/* SPDX-License-Identifier: MIT */
#include <aster/aster.h>
#include <stddef.h>
void *memcpy(void *p,const void *q,size_t n) {unsigned char *d=p;const unsigned char *s=q;while(n--)*d++=*s++;return p;}
void *memset(void *p,int v,size_t n) {unsigned char *d=p;while(n--)*d++=v;return p;}
static aster_handle_t surface;
int32_t aster_app_init(void)
{
    aster_surface_desc_t desc={sizeof(desc),1,0,0,ASTER_PIXEL_RGB565,ASTER_SURFACE_F_PANEL_CANVAS};
    int r=aster_surface_create(&desc,sizeof(desc),&surface);if(r)return r;
    aster_surface_buffer_t buffer; r=aster_surface_acquire(surface,&buffer,sizeof(buffer));if(r)return r;
    if(!buffer.ptr || buffer.flags&ASTER_SURFACE_F_PANEL_CANVAS)return ASTER_EINVAL;
    uint16_t *pixels=buffer.ptr;
    for(unsigned i=0;i<buffer.width*buffer.height;i++)pixels[i]=0x07e0;
    return aster_surface_present(surface);
}
void aster_app_start(void) {aster_app_exit(0);}
void aster_app_event(const aster_event_t *event) {(void)event;}
void aster_app_stop(void) {}
void aster_app_destroy(void) {}
