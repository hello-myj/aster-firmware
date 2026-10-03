/* SPDX-License-Identifier: MIT
 * Fused transform/blend for FLUX's opaque RGB565 ribbons.
 * LVGL's general path writes color and alpha to PSRAM scratch buffers, then
 * reads them back to blend. These ribbons need neither a scratch nor a mask.
 * All other images and rendering features fall back to the stock renderer.
 */
#include <lvgl.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "draw/lv_draw_private.h"
#include "misc/lv_area_private.h"
#include "flux_draw.h"
#include "aster_mem.h"
#if defined(CONFIG_SOC_SERIES_ESP32S3)
#include <esp_attr.h>
#define FLUX_FAST IRAM_ATTR __attribute__((optimize("O3")))
#else
#define FLUX_FAST
#endif
static struct {const void *source;uint8_t *detail;uint16_t color;uint8_t first[18],last[18];} prepared[5];
void aster_flux_draw_forget(const void *source)
{
 for(unsigned i=0;i<5;i++) if(prepared[i].source==source) {
  aster_psram_free(prepared[i].detail);memset(&prepared[i],0,sizeof(prepared[i]));
 }
}
void aster_flux_draw_prepare(const void *source,uint32_t stride)
{
 aster_flux_draw_forget(source);
 for(unsigned i=0;i<5;i++) if(!prepared[i].source) {
  uint8_t *map=aster_psram_alloc(45*140+102);if(!map) return;
  memset(map,0,45*140+102);uint16_t color=*(const uint16_t *)source;
  for(unsigned y=0;y<139;y++) {
   const uint16_t *a=(const uint16_t *)((const uint8_t *)source+y*stride);
   const uint16_t *b=(const uint16_t *)((const uint8_t *)source+(y+1)*stride);
   for(unsigned x=0;x<359;x++) if(a[x]!=color || a[x+1]!=color || b[x]!=color || b[x+1]!=color) {
    map[y*45+x/8]|=1u<<(x&7);
    unsigned tile=(y/8)*45+x/8;
    map[45*140+tile/8]|=1u<<(tile&7);
   }
  }
  for(unsigned band=0;band<18;band++) {
   prepared[i].first[band]=45;prepared[i].last[band]=0;
   for(unsigned col=0;col<45;col++) {
    unsigned tile=band*45+col;
    if(map[45*140+tile/8]&(1u<<(tile&7))) {
     if(prepared[i].first[band]==45) prepared[i].first[band]=col;
     prepared[i].last[band]=col+1;
    }
   }
  }
  prepared[i].source=source;prepared[i].detail=map;prepared[i].color=color;return;
 }
}
static uint32_t draw_cycles;
uint32_t aster_flux_draw_time_reset(void)
{
 uint32_t us=k_cyc_to_us_floor64(draw_cycles);draw_cycles=0;return us;
}

static inline uint16_t swap16(uint16_t v) {return (v << 8) | (v >> 8);}
static inline uint16_t mix565(uint16_t fg, uint16_t bg, unsigned mix)
{
 uint32_t b=(bg | ((uint32_t)bg<<16)) & 0x07e0f81f;
 uint32_t f=(fg | ((uint32_t)fg<<16)) & 0x07e0f81f;
 uint32_t r=((((f-b)*mix)>>5)+b) & 0x07e0f81f;
 return (uint16_t)(r | (r>>16));
}
int FLUX_FAST aster_flux_draw_image(int transformed,int format,const void *source,
                         const void *coordinates,uint32_t stride,
                         const void *clipped,void *task,const void *descriptor)
{
 lv_draw_task_t *t=task;
 const lv_draw_image_dsc_t *d=descriptor;
 const lv_area_t *coords=coordinates;
 lv_layer_t *layer=t->target_layer;
 if(!transformed || format!=LV_COLOR_FORMAT_RGB565 ||
    d->recolor_opa>LV_OPA_MIN || d->colorkey || d->clip_radius ||
    d->blend_mode!=LV_BLEND_MODE_NORMAL || !d->scale_x || !d->scale_y ||
    lv_area_get_width(coords)!=360 || lv_area_get_height(coords)!=140 ||
    !layer->draw_buf || layer->color_format!=LV_COLOR_FORMAT_RGB565_SWAPPED)
  return LV_RESULT_INVALID;
 lv_area_t area;
 if(!lv_area_intersect(&area,clipped,&t->clip_area) ||
    !lv_area_intersect(&area,&area,&layer->buf_area)) return LV_RESULT_OK;
 int width=lv_area_get_width(&area);
 if(width>480) return LV_RESULT_INVALID;
 uint32_t started=k_cycle_get_32();
 /* Write directly into the cached framebuffer; an intermediate scanline
  * would add two PSRAM copies for every transformed bounding rectangle. */

 /* Match LVGL's interpolated 1/10-degree inverse rotation. */
 int angle=-d->rotation,low=angle/10,rem=angle-low*10;
 int sn=(lv_trigo_sin(low)*(10-rem)+lv_trigo_sin(low+1)*rem)/10;
 int cs=(lv_trigo_sin(low+90)*(10-rem)+lv_trigo_sin(low+91)*rem)/10;
 sn>>=LV_TRIGO_SHIFT-10;cs>>=LV_TRIGO_SHIFT-10;
 int32_t dxs=cs*16384/d->scale_x,dys=sn*16384/d->scale_y;
 unsigned mix=((unsigned)d->opa+4)>>3;
 const uint8_t *detail=NULL,*first=NULL,*last=NULL;uint16_t solid=0;
 for(unsigned i=0;i<5;i++) if(prepared[i].source==source) {detail=prepared[i].detail;solid=prepared[i].color;first=prepared[i].first;last=prepared[i].last;break;}
 for(int y=area.y1;y<=area.y2;y++) {
  int x0=area.x1-coords->x1-d->pivot.x;
  int y0=y-coords->y1-d->pivot.y;
  int offset=d->antialias?0:32768;
  int32_t sx=(int64_t)(cs*x0-sn*y0)*16384/d->scale_x+d->pivot.x*65536+offset;
  int32_t sy=(int64_t)(sn*x0+cs*y0)*16384/d->scale_y+d->pivot.y*65536+offset;
  uint16_t *out=(uint16_t *)(layer->draw_buf->data+
    (y-layer->buf_area.y1)*layer->draw_buf->header.stride)+area.x1-layer->buf_area.x1;
  uint16_t *dst=out;
  for(int x=area.x1;x<=area.x2;x++,sx+=dxs,sy+=dys,dst++) {
   int ix=sx>>16,iy=sy>>16;
   uint16_t fg;
   if(d->antialias) {
    /* Skip whole uniform source tiles. Transform stepping and four-tap
     * sampling are needed only near actual artwork and polygon edges. */
    if(detail && dxs>0 && (unsigned)ix<352 && (unsigned)iy<136) {
     unsigned tile=(iy/8)*45+ix/8;
     if(!(detail[45*140+tile/8]&(1u<<(tile&7)))) {
      /* Empty margins extend across many tiles; step to the first
       * detailed tile (or the source edge), rather than eight pixels. */
      unsigned col=ix/8,band=iy/8;
      int end=col<first[band]?first[band]*8:
              col>=last[band]?352:(col+1)*8;
      if(end>352) end=352;
      int run=((end-ix)*65536-(sx&65535)-1)/dxs+1;
      if(dys>0) {
       int r=((8-(iy&7))*65536-(sy&65535)-1)/dys+1;
       if(r<run) run=r;
      } else if(dys<0) {
       int r=((iy&7)*65536+(sy&65535))/(-dys)+1;
       if(r<run) run=r;
      }
      if(run>area.x2-x+1) run=area.x2-x+1;
      if(mix>=32) {
       uint16_t value=swap16(solid);for(int k=0;k<run;k++) dst[k]=value;
      } else for(int k=0;k<run;k++) dst[k]=swap16(mix565(solid,swap16(dst[k]),mix));
      sx+=dxs*(run-1);sy+=dys*(run-1);dst+=run-1;x+=run-1;
      continue;
     }
    }
    if(ix < -1 || ix>=360 || iy < -1 || iy>=140) continue;
    bool inside=(unsigned)ix<359 && (unsigned)iy<139;
    if(inside && detail && !(detail[iy*45+ix/8]&(1u<<(ix&7)))) fg=solid;
    else {
     unsigned wx=(sx>>11)&31,wy=(sy>>11)&31;uint16_t pixels[4];
     if(inside) {
      const uint16_t *a=(const uint16_t *)((const uint8_t *)source+iy*stride);
      const uint16_t *b=(const uint16_t *)((const uint8_t *)source+(iy+1)*stride);
      pixels[0]=a[ix];pixels[1]=a[ix+1];pixels[2]=b[ix];pixels[3]=b[ix+1];
     } else {
      uint16_t bg=swap16(*dst);
      for(int v=0;v<2;v++) for(int u=0;u<2;u++) {
       int xx=ix+u,yy=iy+v;
       pixels[v*2+u]=(unsigned)xx<360 && (unsigned)yy<140?
         ((const uint16_t *)((const uint8_t *)source+yy*stride))[xx]:bg;
      }
     }
     if(pixels[0]==pixels[1] && pixels[0]==pixels[2] && pixels[0]==pixels[3]) fg=pixels[0];
     else fg=mix565(mix565(pixels[0],pixels[1],32-wx),mix565(pixels[2],pixels[3],32-wx),32-wy);
    }
   } else {
    if((unsigned)ix>=360 || (unsigned)iy>=140) continue;
    fg=((const uint16_t *)((const uint8_t *)source+iy*stride))[ix];
   }
   *dst=swap16(mix>=32?fg:mix565(fg,swap16(*dst),mix));
  }

 }
 draw_cycles+=(uint32_t)(k_cycle_get_32()-started);
 return LV_RESULT_OK;
}
