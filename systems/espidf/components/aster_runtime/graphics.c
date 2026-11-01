/* SPDX-License-Identifier: MIT */
#include "graphics.h"
#ifdef ESP_PLATFORM
#include <zephyr/sys/util.h>
#include "ui_gfx.h"
#include "ui_font.h"
#include "font_sd.h"
#include "aster_mem.h"
#include "files.h"
#include "gfx_rgb888.h"
#include <lvgl.h>
#include <stdio.h>
#endif
#include <string.h>
#include <limits.h>
#include <stdlib.h>

static aster_idf_io_t io;
static const uint8_t *(*capture_callback)(void);
void aster_idf_set_capture(const uint8_t *(*capture)(void)) {capture_callback=capture;}
const uint8_t *aster_idf_capture(void) {return capture_callback?capture_callback():NULL;}
static int guest_present(const uint8_t *p,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch)
{return aster_idf_app_glass_frozen()?0:io.present(p,x,y,w,h,pitch);}
static struct {
    uint32_t handle, pixels, width, height, format, bpp;
    bool scale_fit;
} surface;
static uint8_t *scaled_pixels;
int aster_idf_present_bgr888(const uint8_t *p,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch)
{
    if(!io.present_bgr888)return ASTER_ENOTSUP;
    if(!p || !w || !h || x>=io.width || y>=io.height || w>io.width-x || h>io.height-y || pitch<w)return ASTER_EINVAL;
    return io.present_bgr888(p,x,y,w,h,pitch)?ASTER_EIO:0;
}
const uint8_t *aster_idf_capture_bgr888(void) {return io.capture_bgr888?io.capture_bgr888():NULL;}
static uint32_t native_format(void) {return io.present_bgr888?ASTER_PIXEL_BGR888:ASTER_PIXEL_RGB565;}
static uint64_t formats(void) {return ASTER_FMT_BIT(ASTER_PIXEL_RGB565)|(io.present_bgr888?ASTER_FMT_BIT(ASTER_PIXEL_RGB888):0);}
static int surface_present(const uint8_t *p,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch)
{
    if(aster_idf_app_glass_frozen())return 0;
    if(surface.format==ASTER_PIXEL_RGB565)return guest_present(p,x,y,w,h,pitch);
    if(surface.format==ASTER_PIXEL_BGR888)return aster_idf_present_bgr888(p,x,y,w,h,pitch);
    uint8_t *copy=malloc((size_t)w*h*3);if(!copy)return ASTER_ENOMEM;
    for(uint32_t row=0;row<h;row++)for(uint32_t col=0;col<w;col++) {
        const uint8_t *a=p+((size_t)row*pitch+col)*3;uint8_t *b=copy+((size_t)row*w+col)*3;
        b[0]=a[2];b[1]=a[1];b[2]=a[0];
    }
    int result=aster_idf_present_bgr888(copy,x,y,w,h,w);free(copy);return result;
}
static uint32_t serial;
static aster_idf_touch_point_t contacts[ASTER_IDF_MAX_TOUCH];
static uint32_t contact_mask, delivered_mask;
static uint32_t actions[ASTER_IDF_MAX_TOUCH];
static aster_event_t touch_queue[ASTER_IDF_MAX_TOUCH];
static unsigned touch_next, touch_count;
static bool has_touch(void) { return io.touch || io.touch_frame; }
static uint32_t touch_capacity(void) {
    return io.touch_frame ? io.max_touch_points : (io.touch ? 1 : 0);
}
uint32_t aster_idf_touch_mask(void) { return delivered_mask; }
bool aster_idf_touch_pending(void) { return touch_next < touch_count; }
void aster_idf_graphics_reset_touch(void) {
    contact_mask=delivered_mask=touch_next=touch_count=0;
    memset(contacts,0,sizeof(contacts));memset(actions,0,sizeof(actions));
}

void aster_idf_set_io(const aster_idf_io_t *value)
{
    io = value ? *value : (aster_idf_io_t){0};
    /* Bound all multiplication used by the ABI, even with an invalid backend. */
    if (!io.width || !io.height || io.width > 480 || io.height > 480)
        memset(&io, 0, sizeof(io));
    if(io.touch_frame && (!io.max_touch_points || io.max_touch_points>ASTER_IDF_MAX_TOUCH))
        io.touch_frame=NULL;
    aster_idf_graphics_reset_touch();
}

int aster_idf_display_count(void) { return io.present ? 1 : 0; }
int aster_idf_feature(uint32_t bit)
{
    if (bit == ASTER_FEATURE_DISPLAY || bit == ASTER_FEATURE_FRAMEBUFFER)
        return io.present != NULL;
    if (bit == ASTER_FEATURE_TOUCH) return has_touch();
    return 0;
}

static void *guest(wasm_module_inst_t instance, uint32_t offset, uint32_t bytes)
{
    uint64_t start, end;
    if (!offset || !wasm_runtime_get_app_addr_range(instance, offset, &start, &end) ||
        offset < start || offset > end || bytes > end-offset) return NULL;
    return wasm_runtime_addr_app_to_native(instance, offset);
}
#define INST wasm_runtime_get_module_inst(env)
static void fit_geometry(uint32_t *w,uint32_t *h,uint32_t *x,uint32_t *y)
{
    if((uint64_t)io.width*surface.height<=(uint64_t)io.height*surface.width) {
        *w=io.width;*h=(uint64_t)surface.height*io.width/surface.width;if(!*h)*h=1;
    } else {*h=io.height;*w=(uint64_t)surface.width*io.height/surface.height;if(!*w)*w=1;}
    *x=(io.width-*w)/2;*y=(io.height-*h)/2;
}
void aster_idf_map_touch(aster_event_t *event)
{
    if(!surface.scale_fit)return;
    uint32_t w,h,x,y;fit_geometry(&w,&h,&x,&y);
    uint32_t tx=event->u.touch.x<x?0:(uint32_t)event->u.touch.x-x;
    uint32_t ty=event->u.touch.y<y?0:(uint32_t)event->u.touch.y-y;
    event->u.touch.x=tx>=w?surface.width-1:(uint64_t)tx*surface.width/w;
    event->u.touch.y=ty>=h?surface.height-1:(uint64_t)ty*surface.height/h;
}
static int present_scaled(wasm_module_inst_t instance)
{
    const uint8_t *source=guest(instance,surface.pixels,surface.width*surface.height*surface.bpp);
    if(!source || !scaled_pixels)return ASTER_EINVAL;
    uint32_t w,h,x,y;fit_geometry(&w,&h,&x,&y);
    memset(scaled_pixels,0,(size_t)io.width*io.height*surface.bpp);
    for(uint32_t row=0;row<h;row++)for(uint32_t col=0;col<w;col++)
        memcpy(scaled_pixels+((size_t)(y+row)*io.width+x+col)*surface.bpp,
            source+((uint64_t)row*surface.height/h*surface.width+(uint64_t)col*surface.width/w)*surface.bpp,surface.bpp);
    return surface_present(scaled_pixels,0,0,io.width,io.height,io.width);

}

static int32_t display_info(wasm_exec_env_t env, uint32_t index, uint32_t off, uint32_t bytes)
{
    if (index || !io.present) return ASTER_ENODEV;
    if (bytes < sizeof(aster_display_info_t)) return ASTER_EINVAL;
    void *dst = guest(INST, off, sizeof(aster_display_info_t));
    if (!dst) return ASTER_EINVAL;
    aster_display_info_t value = {
        .size=sizeof(value), .version=1, .width=io.width, .height=io.height,
        .app_width=surface.scale_fit?surface.width:io.width, .app_height=surface.scale_fit?surface.height:io.height,
        .native_format=native_format(),
        .supported_formats=formats(),
        .orientation=ASTER_ORIENT_PORTRAIT,
    };
    memcpy(dst, &value, sizeof(value));
    return ASTER_OK;
}

static int32_t create(wasm_exec_env_t env, uint32_t off, uint32_t bytes, uint32_t out)
{
    if (!io.present) return ASTER_ENODEV;
    if (bytes < sizeof(aster_surface_desc_t)) return ASTER_EINVAL;
    void *src = guest(INST, off, sizeof(aster_surface_desc_t));
    if (!src || !guest(INST, out, 4)) return ASTER_EINVAL;
    aster_surface_desc_t desc;
    memcpy(&desc, src, sizeof(desc));
    if (desc.size < sizeof(desc) || desc.version != 1) return ASTER_EINVAL;
    if(desc.flags&~(ASTER_SURFACE_F_PANEL_CANVAS|ASTER_SURFACE_F_SCALE_FIT))return ASTER_EINVAL;
    bool fit=(desc.flags&ASTER_SURFACE_F_SCALE_FIT)!=0;
    if(fit && ((desc.flags&ASTER_SURFACE_F_PANEL_CANVAS) || !desc.width || !desc.height))return ASTER_EINVAL;
    /* PANEL_CANVAS is a hint. Decline it with an ordinary guest buffer. */
    uint32_t format=desc.format==ASTER_PIXEL_NATIVE?native_format():desc.format;
    if(format!=ASTER_PIXEL_RGB565 && (!io.present_bgr888 || (format!=ASTER_PIXEL_RGB888 && format!=ASTER_PIXEL_BGR888)))return ASTER_ENOTSUP;
    uint32_t bpp=format==ASTER_PIXEL_RGB565?2:3;
    uint32_t width = desc.width ? desc.width : io.width;
    uint32_t height = desc.height ? desc.height : io.height;
    if(!width || !height || width>4096 || height>4096 || (uint64_t)width*height*bpp>16*1024*1024 ||
       (!fit && (width>io.width || height>io.height)))return ASTER_EINVAL;
    if (surface.handle) return ASTER_EBUSY;
    void *native = NULL;
    uint32_t pixels = wasm_runtime_module_malloc(INST, width * height * bpp, &native);
    if (!pixels) return ASTER_ENOMEM;
    if(fit) {
#ifdef ESP_PLATFORM
        scaled_pixels=aster_psram_alloc((size_t)io.width*io.height*bpp);
#else
        scaled_pixels=malloc((size_t)io.width*io.height*bpp);
#endif
        if(!scaled_pixels){wasm_runtime_module_free(INST,pixels);return ASTER_ENOMEM;}
    }
    /* module_malloc can grow guest memory: reacquire all output addresses. */
    void *dst = guest(INST, out, 4);
    if (!dst) { wasm_runtime_module_free(INST, pixels);free(scaled_pixels);scaled_pixels=NULL;return ASTER_EINVAL; }
    memset(native, 0, width * height * bpp);
    serial=(serial & 0x00ffffffu)+1;
    if (serial>0x00ffffffu)serial=1;
    serial |= ASTER_HTYPE_SURFACE;
    surface.handle=serial; surface.pixels=pixels; surface.width=width; surface.height=height;
    surface.scale_fit=fit;surface.format=format;surface.bpp=bpp;
    memcpy(dst, &serial, 4);
    return ASTER_OK;
}

static int32_t destroy(wasm_exec_env_t env, uint32_t handle)
{
    if (!handle || handle != surface.handle) return ASTER_EBADF;
    wasm_runtime_module_free(INST, surface.pixels);
    free(scaled_pixels);scaled_pixels=NULL;
    memset(&surface, 0, sizeof(surface));
    return ASTER_OK;
}

static int32_t acquire(wasm_exec_env_t env, uint32_t handle, uint32_t off, uint32_t bytes)
{
    if (!handle || handle != surface.handle) return ASTER_EBADF;
    /* Wire representation is always WASM32, including when tests run on x86_64. */
    struct { uint32_t size, version, ptr, width, height, pitch, format, flags; } value = {
        32, 1, surface.pixels, surface.width, surface.height, surface.width*surface.bpp, surface.format, surface.scale_fit?ASTER_SURFACE_F_SCALE_FIT:0
    };
    if (bytes < sizeof(value)) return ASTER_EINVAL;
    void *dst = guest(INST, off, sizeof(value));
    if (!dst) return ASTER_EINVAL;
    memcpy(dst, &value, sizeof(value));
    return ASTER_OK;
}

static int32_t flush(wasm_exec_env_t env, uint32_t handle, uint32_t off,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pitch)
{
    if (!handle || handle != surface.handle) return ASTER_EBADF;
    if (!w || !h || x >= surface.width || y >= surface.height ||
        w > surface.width-x || h > surface.height-y || pitch < w) return ASTER_EINVAL;
    uint64_t span = ((uint64_t)(h-1)*pitch + w)*surface.bpp;
    if (span > UINT32_MAX) return ASTER_EINVAL;
    void *src = guest(INST, off, (uint32_t)span);
    if (!src) return ASTER_EINVAL;
    if(surface.scale_fit) {
        void *destination=guest(INST,surface.pixels,surface.width*surface.height*surface.bpp);
        if(!destination)return ASTER_EINVAL;
#ifdef ESP_PLATFORM
        uint8_t *copy=aster_psram_alloc((size_t)w*h*surface.bpp);
#else
        uint8_t *copy=malloc((size_t)w*h*surface.bpp);
#endif
        if(!copy)return ASTER_ENOMEM;
        for(uint32_t row=0;row<h;row++)memcpy(copy+(size_t)row*w*surface.bpp,(uint8_t *)src+(size_t)row*pitch*surface.bpp,w*surface.bpp);
        for(uint32_t row=0;row<h;row++)memcpy((uint8_t *)destination+((size_t)(y+row)*surface.width+x)*surface.bpp,copy+(size_t)row*w*surface.bpp,w*surface.bpp);
        free(copy);return present_scaled(INST);
    }
    return surface_present(src, x, y, w, h, pitch);
}

static int32_t present(wasm_exec_env_t env, uint32_t handle)
{
    if (!handle || handle != surface.handle) return ASTER_EBADF;
    if(surface.scale_fit)return present_scaled(INST);
    return flush(env, handle, surface.pixels, 0, 0, surface.width, surface.height, surface.width);
}

static int32_t input_info(wasm_exec_env_t env, uint32_t off, uint32_t bytes)
{
    if (bytes < sizeof(aster_input_info_t)) return ASTER_EINVAL;
    void *dst = guest(INST, off, sizeof(aster_input_info_t));
    if (!dst) return ASTER_EINVAL;
    aster_input_info_t value = {.size=sizeof(value), .version=1,
        .kinds=has_touch() ? ASTER_INPUT_TOUCH : 0, .max_points=touch_capacity()};
    memcpy(dst, &value, sizeof(value));
    return ASTER_OK;
}

bool aster_idf_touch_event(aster_event_t *event)
{
    if (!has_touch()) return false;
    if(touch_next==touch_count) {
        aster_idf_touch_point_t next[ASTER_IDF_MAX_TOUCH]={0};
        int mask=io.touch_frame ? io.touch_frame(next) : io.touch(&next[0].x,&next[0].y);
        if(mask<0)return false; /* No frame / I/O error must not release fingers. */
        if((uint32_t)mask & ~((1u<<touch_capacity())-1u))return false;
        for(unsigned slot=0;slot<touch_capacity();slot++)
            if((mask&(1u<<slot)) && (next[slot].x>=io.width || next[slot].y>=io.height))return false;
        touch_next=touch_count=0;
        for(unsigned slot=0;slot<touch_capacity();slot++) {
            bool was=(contact_mask&(1u<<slot))!=0, now=(mask&(1u<<slot))!=0;
            uint32_t action;
            if(now) {
                action=was?ASTER_TOUCH_MOVE:ASTER_TOUCH_DOWN;
                if(was && next[slot].x==contacts[slot].x && next[slot].y==contacts[slot].y)continue;
                contacts[slot]=next[slot];
            } else {
                if(!was)continue;
                action=ASTER_TOUCH_UP;
            }
            actions[slot]=action;
            aster_event_t *out=&touch_queue[touch_count++];
            memset(out,0,sizeof(*out));out->size=sizeof(*out);out->version=1;out->type=ASTER_EVENT_TOUCH;
            out->u.touch.x=contacts[slot].x;out->u.touch.y=contacts[slot].y;
            out->u.touch.action=action;out->u.touch.slot=slot;
        }
        contact_mask=(uint32_t)mask;
        if(!touch_count)return false;
    }
    *event=touch_queue[touch_next++];delivered_mask=contact_mask;
    return true;
}

static int32_t touch(wasm_exec_env_t env, uint32_t off, uint32_t max)
{
    if (!has_touch() || !max || !contact_mask) return 0;
    aster_touch_point_t values[ASTER_IDF_MAX_TOUCH];unsigned count=0;
    for(unsigned slot=0;slot<touch_capacity() && count<max;slot++) {
        if(!(contact_mask&(1u<<slot)))continue;
        aster_event_t event={0};event.u.touch.x=contacts[slot].x;event.u.touch.y=contacts[slot].y;
        aster_idf_map_touch(&event);
        values[count++]=(aster_touch_point_t){event.u.touch.x,event.u.touch.y,actions[slot]};
    }
    void *dst=guest(INST,off,count*sizeof(values[0]));if(!dst)return ASTER_EINVAL;
    memcpy(dst,values,count*sizeof(values[0]));return count;
}

void aster_idf_graphics_cleanup(wasm_module_inst_t instance)
{
    if (surface.handle) wasm_runtime_module_free(instance, surface.pixels);
    free(scaled_pixels);scaled_pixels=NULL;
    memset(&surface, 0, sizeof(surface));
    aster_idf_graphics_reset_touch();
#ifdef ESP_PLATFORM
    aster_gfx_drop_all();
#endif
}

#ifdef ESP_PLATFORM
/* Shared image-decoder display helpers use the same synchronous backend. */
int aster_plat_display_get_info(uint32_t index,aster_display_info_t *out)
{
    if (index || !io.present || !out) return ASTER_ENODEV;
    *out=(aster_display_info_t){.size=sizeof(*out),.version=1,
        .width=io.width,.height=io.height,.app_width=io.width,.app_height=io.height,
        .native_format=native_format(),.supported_formats=formats()};
    return 0;
}
int aster_plat_display_write_at(const uint8_t *p,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch)
{
    if (!io.present) return ASTER_ENODEV;
    if (!p || !w || !h || x>=io.width || y>=io.height || w>io.width-x || h>io.height-y || pitch<w)
        return ASTER_EINVAL;
    return io.present(p,x,y,w,h,pitch)==0 ? 0 : ASTER_EIO;
}
int aster_plat_display_clear(uint16_t color)
{
    uint16_t line[480];for(unsigned x=0;x<io.width;x++)line[x]=color;
    for(unsigned y=0;y<io.height;y++) {
        int r=aster_plat_display_write_at((uint8_t *)line,0,y,io.width,1,io.width);
        if(r)return r;
    }
    return io.present ? 0 : ASTER_ENODEV;
}
void aster_plat_display_flush_drain(void) { /* present() already completes every source read. */ }

static int32_t gfx_exec(wasm_exec_env_t env, uint32_t handle, uint32_t cmd_off,
                        uint32_t n, uint32_t arena_off, uint32_t arena_len,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!handle || handle!=surface.handle) return ASTER_EBADF;
    if(n==ASTER_GFX_FRAME_CONTROL) {
        if(!io.present_bgr888)return ASTER_ENOTSUP;
        if(x!=ASTER_GFX_FRAME_MAGIC || y!=1 || w || h || arena_len!=sizeof(aster_gfx_frame_control_t))return ASTER_EINVAL;
        aster_gfx_frame_control_t *dst=guest(INST,arena_off,sizeof(*dst)),control;
        if(!dst)return ASTER_EINVAL;
        memcpy(&control,dst,sizeof(control));
        if(control.size!=sizeof(control) || control.version!=1 || control.action!=ASTER_GFX_FRAME_QUERY || control.frame_id || control.flags || control.error)return ASTER_EINVAL;
        control.flags=ASTER_GFX_RGB888_SUPPORTED;memcpy(dst,&control,sizeof(control));return 0;
    }
    if(n==ASTER_GFX_RGB888_BATCH) {
        if(surface.bpp!=3)return ASTER_ENOTSUP;
        if(!x || x>256 || y!=1 || w || h || arena_len>ASTER_GFX_FRAME_MAX_ARENA)return ASTER_EINVAL;
        const void *input=guest(INST,cmd_off,x*sizeof(aster_gfx_rgb888_cmd_t));
        const void *text=arena_len?guest(INST,arena_off,arena_len):NULL;
        if(!input || (arena_len && !text))return ASTER_EINVAL;
        aster_gfx_rgb888_cmd_t *commands=malloc(x*sizeof(*commands));uint8_t *arena=arena_len?malloc(arena_len):NULL;
        if(!commands || (arena_len && !arena)){free(commands);free(arena);return ASTER_ENOMEM;}
        memcpy(commands,input,x*sizeof(*commands));if(arena_len)memcpy(arena,text,arena_len);
        uint8_t *pixels=guest(INST,surface.pixels,surface.width*surface.height*3);
        int result=pixels?aster_idf_gfx888(pixels,surface.width,surface.height,surface.format==ASTER_PIXEL_RGB888,commands,x,arena,arena_len):ASTER_EINVAL;
        free(commands);free(arena);
        if(result>=0){int error=surface.scale_fit?present_scaled(INST):surface_present(pixels,0,0,surface.width,surface.height,surface.width);if(error)result=error;}
        return result;
    }
    if(surface.bpp==3)return ASTER_ENOTSUP;
    if (n>256 || arena_len>ASTER_GFX_FRAME_MAX_ARENA) return ASTER_EINVAL;
    if ((w==0)!=(h==0) || (w && (x>=surface.width || y>=surface.height ||
        w>surface.width-x || h>surface.height-y))) return ASTER_EINVAL;
    if (!n) return 0;
    const void *source=guest(INST,cmd_off,n*sizeof(aster_gfx_cmd_t));
    const void *arena_source=arena_len ? guest(INST,arena_off,arena_len) : NULL;
    if (!source || (arena_len && !arena_source)) return ASTER_EINVAL;
    aster_gfx_cmd_t *commands=aster_psram_alloc(n*sizeof(*commands));
    uint8_t *arena=arena_len ? aster_psram_alloc(arena_len) : NULL;
    if (!commands || (arena_len && !arena)) {
        aster_psram_free(commands);aster_psram_free(arena);return ASTER_ENOMEM;
    }
    memcpy(commands,source,n*sizeof(*commands));
    if (arena_len) memcpy(arena,arena_source,arena_len);
    int result=ASTER_EINVAL;
    /* Validate the entire batch before drawing, including aliased input. */
    for (uint32_t i=0;i<n;i++) {
        uint32_t op=ASTER_GFX_OP(commands+i);
        if (op==8 || op>ASTER_GFX_BMP_SA) goto done;
        if (op==ASTER_GFX_TEXT && (commands[i].w[3]>arena_len ||
            ASTER_GFX_U16(commands[i].w[4])>arena_len-commands[i].w[3])) goto done;
    }
    uint8_t *pixels=guest(INST,surface.pixels,surface.width*surface.height*2);
    if (!pixels) goto done;
    uint32_t dx=0,dy=0,dw=0,dh=0;
    aster_gfx_set_clip(x,y,w,h);
    result=aster_gfx_exec_into(pixels,surface.width,surface.height,surface.width,
        commands,n,arena,arena_len,&dx,&dy,&dw,&dh,false);
    aster_gfx_set_clip(0,0,0,0);
    if (result>=0) {
        if (!w) { x=dx;y=dy;w=dw;h=dh; }
        if(w && h) {
            if(surface.scale_fit) {if(present_scaled(INST))result=ASTER_EIO;}
            else if(guest_present(pixels+((size_t)y*surface.width+x)*2,x,y,w,h,surface.width)!=0)result=ASTER_EIO;
        }
    }
done:
    aster_psram_free(commands);aster_psram_free(arena);
    return result;
}
static int32_t text_width(wasm_exec_env_t env,uint32_t px,uint32_t off,uint32_t len)
{
    if (px>255 || len>65535) return ASTER_EINVAL;
    const char *text=len ? guest(INST,off,len) : "";
    if (!text) return ASTER_EINVAL;
    return aster_gfx_text_measure(px,text,len);
}
static int32_t fonts_pending(wasm_exec_env_t env)
{ (void)env;return aster_ui_fonts_poll(); }
static int32_t bmp_load(wasm_exec_env_t env,uint32_t off,uint32_t len,uint32_t cw,
                        uint32_t ch,uint32_t oh,uint32_t ow,uint32_t out_height)
{
    if (!len || (len>=192 && len!=UINT32_MAX) || !cw || !ch || cw>480 || ch>480) return ASTER_EINVAL;
    if (!guest(INST,oh,4) || !guest(INST,ow,4) || !guest(INST,out_height,4))return ASTER_EINVAL;
    char real[256];int resolved=aster_idf_resolve_path(env,off,len,false,real);
    if(resolved)return resolved;
    uint32_t handle,width,height;
    int result=aster_gfx_bmp_load_into(real,cw,ch,&handle,&width,&height);
    if (result) return result;
    memcpy(guest(INST,oh,4),&handle,4);memcpy(guest(INST,ow,4),&width,4);
    memcpy(guest(INST,out_height,4),&height,4);
    return 0;
}
static int32_t bmp_free(wasm_exec_env_t env,uint32_t handle)
{ (void)env;return aster_gfx_bmp_release(handle); }
#endif

static NativeSymbol symbols[] = {
#ifdef ESP_PLATFORM
    {"aster_gfx_exec",(void *)gfx_exec,"(iiiiiiiii)i",NULL},
    {"aster_gfx_text_w",(void *)text_width,"(iii)i",NULL},
    {"aster_gfx_fonts_pending",(void *)fonts_pending,"()i",NULL},
    {"aster_gfx_bmp_load",(void *)bmp_load,"(iiiiiii)i",NULL},
    {"aster_gfx_bmp_free",(void *)bmp_free,"(i)i",NULL},
#endif
    {"aster_display_get_info", (void *)display_info, "(iii)i", NULL},
    {"aster_surface_create", (void *)create, "(iii)i", NULL},
    {"aster_surface_destroy", (void *)destroy, "(i)i", NULL},
    {"aster_surface_acquire", (void *)acquire, "(iii)i", NULL},
    {"aster_surface_present", (void *)present, "(i)i", NULL},
    {"aster_surface_flush", (void *)flush, "(iiiiiii)i", NULL},
    {"aster_input_get_info", (void *)input_info, "(ii)i", NULL},
    {"aster_input_touch", (void *)touch, "(ii)i", NULL},
};
NativeSymbol *aster_idf_graphics_symbols(uint32_t *count)
{
    *count = sizeof(symbols)/sizeof(symbols[0]);
    return symbols;
}
