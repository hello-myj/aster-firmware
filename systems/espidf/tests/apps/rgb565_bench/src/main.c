/* SPDX-License-Identifier: MIT */
/* Physical AOT frame pacing / PPA coherence check. No pixel readback is
 * included in the timed draw. The final frame checks every guest pixel. */
#include <aster_imports.h>
#include <aster/gfx.h>
static aster_handle_t surface;
static aster_surface_buffer_t buffer;
static aster_gfx_cmd_t commands[18];
static uint32_t frames,total_ms,max_ms,interval_ms,last_ms,errors;
ASTER_APP_EXPORT int aster_app_init(void)
{
    aster_surface_desc_t desc={.size=sizeof(desc),.version=1,.format=ASTER_PIXEL_RGB565};
    int result=aster_surface_create(&desc,sizeof(desc),&surface);
    if(result)return result;
    buffer.size=sizeof(buffer);buffer.version=1;
    result=aster_surface_acquire(surface,&buffer,sizeof(buffer));
    if(result)return result;
    commands[0].w[0]=ASTER_GFX_CLEAR;commands[0].w[1]=0x0841;
    for(unsigned i=0;i<16;i++) {
        aster_gfx_cmd_t *c=&commands[i+1];c->w[0]=ASTER_GFX_RECT_F;
        c->w[1]=ASTER_GFX_PACK(8+(i%8)*(buffer.width/8),20+(i/8)*(buffer.height/3));
        c->w[2]=ASTER_GFX_PACK(buffer.width/10,buffer.height/5);
    }
    /* Partial cache-line boundaries and clipping; small CPU fallback writes
     * into a surface previously filled by PPA. */
    commands[17].w[0]=ASTER_GFX_RECT_F;
    commands[17].w[1]=ASTER_GFX_PACK(-3,buffer.height-9);
    commands[17].w[2]=ASTER_GFX_PACK(24,16);commands[17].w[3]=0xf81f;
    aster_app_set_framerate(60);return 0;
}
static uint32_t check_pixels(void)
{
    aster_surface_acquire(surface,&buffer,sizeof(buffer));
    const uint16_t *data=buffer.ptr;uint32_t bad=0;
    for(uint32_t y=0;y<buffer.height;y++)for(uint32_t x=0;x<buffer.width;x++) {
        uint16_t expected=commands[0].w[1];
        for(unsigned i=1;i<18;i++) {
            const aster_gfx_cmd_t *c=&commands[i];
            int32_t cx=ASTER_GFX_P16(c->w[1]),cy=ASTER_GFX_P16H(c->w[1]);
            if((int32_t)x>=cx && (int32_t)y>=cy &&
               (int32_t)x<cx+ASTER_GFX_U16(c->w[2]) && (int32_t)y<cy+ASTER_GFX_HI16(c->w[2]))
                expected=(uint16_t)c->w[3];
        }
        if(data[y*(buffer.pitch/2)+x]!=expected)bad++;
    }
    return bad;
}
ASTER_APP_EXPORT void aster_app_event(const aster_event_t *event)
{
    if(event->type!=ASTER_EVENT_FRAME || frames>=128)return;
    uint32_t now=aster_time_now_ms();if(frames)interval_ms+=now-last_ms;last_ms=now;
    for(unsigned i=0;i<16;i++)commands[i+1].w[3]=(i*3001+frames*97+0x1f)&65535;
    if(aster_gfx_exec(surface,commands,18,0,0,0,0,0,0)!=18)errors++;
    uint32_t elapsed=aster_time_now_ms()-now;total_ms+=elapsed;if(elapsed>max_ms)max_ms=elapsed;
    if(++frames==128) {
        uint32_t bad=check_pixels();
        aster_logf(ASTER_LOG_INFO,"RGB565BENCH frames=%u draw_total_ms=%u draw_max_ms=%u interval_total_ms=%u intervals=127 errors=%u pixel_errors=%u",frames,total_ms,max_ms,interval_ms,errors,bad);
    }
}
ASTER_APP_EXPORT void aster_app_stop(void){if(surface)aster_surface_destroy(surface);surface=0;}
