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
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4)
#include <esp_attr.h>
#define FLUX_FAST IRAM_ATTR __attribute__((optimize("O3")))
#else
#define FLUX_FAST
#endif
#ifdef ASTER_SYSTEM_ESP_IDF
static aster_flux_parallel_fn parallel_rows;
void aster_flux_set_parallel(aster_flux_parallel_fn executor) {parallel_rows=executor;}
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
 if(stride<360u*2u) return;
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
static inline void splat565(uint16_t *p,int count,uint16_t value)
{
 if(((uintptr_t)p&2) && count){*p++=value;count--;}
 uint32_t pair=(uint32_t)value|((uint32_t)value<<16);
 while(count>=2){*(uint32_t *)p=pair;p+=2;count-=2;}
 if(count)*p=value;
}
static inline uint16_t mix565(uint16_t fg, uint16_t bg, unsigned mix)
{
 uint32_t b=(bg | ((uint32_t)bg<<16)) & 0x07e0f81f;
 uint32_t f=(fg | ((uint32_t)fg<<16)) & 0x07e0f81f;
 uint32_t r=((((f-b)*mix)>>5)+b) & 0x07e0f81f;
 return (uint16_t)(r | (r>>16));
}
/* Interpolate once in each native RGB565 channel. Two successive mixes
 * truncate twice and bias fine edges toward the first sample. Use eight
 * fractional bits per axis and round once, preserving subpixel positions. */
static inline uint16_t sample565(const uint16_t p[4],unsigned wx,unsigned wy)
{
 /* Weights sum to 65536. Express three samples relative to p[0], saving
  * six multiplies per bilinear sample without intermediate rounding. */
 int w3=wx*wy,w1=(int)(wx<<8)-w3,w2=(int)(wy<<8)-w3;
 int r0=p[0]>>11,g0=(p[0]>>5)&63,b0=p[0]&31;
 int r=(r0<<16)+((p[1]>>11)-r0)*w1+((p[2]>>11)-r0)*w2+((p[3]>>11)-r0)*w3;
 int g=(g0<<16)+(((p[1]>>5)&63)-g0)*w1+(((p[2]>>5)&63)-g0)*w2+(((p[3]>>5)&63)-g0)*w3;
 int b=(b0<<16)+((p[1]&31)-b0)*w1+((p[2]&31)-b0)*w2+((p[3]&31)-b0)*w3;
 return (((r+32768)>>16)<<11)|(((g+32768)>>16)<<5)|((b+32768)>>16);
}

typedef struct {
 const void *source;const lv_area_t *coords;uint32_t stride;
 const lv_draw_image_dsc_t *d;lv_layer_t *layer;lv_area_t area;bool swapped;
} flux565_job_t;
static void FLUX_FAST draw565_rows(void *context,int first_y,int last_y)
{
 const flux565_job_t *job=context;const void *source=job->source;
 const lv_area_t *coords=job->coords;uint32_t stride=job->stride;
 const lv_draw_image_dsc_t *d=job->d;lv_layer_t *layer=job->layer;
 lv_area_t area=job->area;bool swapped=job->swapped;
 /* Match LVGL's interpolated 1/10-degree inverse rotation. */
 int angle=-d->rotation,low=angle/10,rem=angle-low*10;
 int sn=(lv_trigo_sin(low)*(10-rem)+lv_trigo_sin(low+1)*rem)/10;
 int cs=(lv_trigo_sin(low+90)*(10-rem)+lv_trigo_sin(low+91)*rem)/10;
 /* Retain LVGL's full trigonometric precision. Reducing it to ten
  * bits introduces position errors before glyph-edge interpolation. */
 const int factor=1<<(24-LV_TRIGO_SHIFT);
 int32_t dxs=cs*factor/d->scale_x,dys=sn*factor/d->scale_y;
 unsigned mix=((unsigned)d->opa+4)>>3;
 const uint8_t *detail=NULL,*first=NULL,*last=NULL;uint16_t solid=0;
 for(unsigned i=0;i<5;i++) if(prepared[i].source==source) {detail=prepared[i].detail;solid=prepared[i].color;first=prepared[i].first;last=prepared[i].last;break;}
 for(int y=first_y;y<=last_y;y++) {
  int x0=area.x1-coords->x1-d->pivot.x;
  int y0=y-coords->y1-d->pivot.y;
  int offset=d->antialias?0:32768;
  int32_t sx=((int64_t)cs*x0-(int64_t)sn*y0)*factor/d->scale_x+d->pivot.x*65536+offset;
  int32_t sy=((int64_t)sn*x0+(int64_t)cs*y0)*factor/d->scale_y+d->pivot.y*65536+offset;
  uint16_t *out=(uint16_t *)(layer->draw_buf->data+
    (y-layer->buf_area.y1)*layer->draw_buf->header.stride)+area.x1-layer->buf_area.x1;
  uint16_t *dst=out;
  for(int x=area.x1;x<=area.x2;x++,sx+=dxs,sy+=dys,dst++) {
   int ix=sx>>16,iy=sy>>16;
   uint16_t fg;
   if(d->antialias) {
    /* Skip whole uniform source tiles. Transform stepping and four-tap
     * sampling are needed only near actual artwork and polygon edges. */
    /* The tile skip was measured for one scale on both axes. A non-uniform
     * fit can produce a zero step and spin on this core. */
    if(detail && d->scale_x==d->scale_y && dxs>0 && (unsigned)ix<352 && (unsigned)iy<136) {
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
      /* run <= 0 makes x += run-1 cancel the for-loop increment and spin. */
      if(run<1) run=1;
      if(run>area.x2-x+1) run=area.x2-x+1;
      if(mix>=32) {
       splat565(dst,run,swapped?swap16(solid):solid);
      } else for(int k=0;k<run;) {
       uint16_t raw=dst[k];int end=k+1;
       while(end<run && dst[end]==raw)end++;
       uint16_t bg=swapped?swap16(raw):raw,value=mix565(solid,bg,mix);
       splat565(dst+k,end-k,swapped?swap16(value):value);k=end;
      }
      sx+=dxs*(run-1);sy+=dys*(run-1);dst+=run-1;x+=run-1;
      continue;
     }
    }
    if(ix < -1 || ix>=360 || iy < -1 || iy>=140) continue;
    bool inside=(unsigned)ix<359 && (unsigned)iy<139;
    if(inside && detail && !(detail[iy*45+ix/8]&(1u<<(ix&7)))) fg=solid;
    else {
     unsigned wx=(sx>>8)&255,wy=(sy>>8)&255;uint16_t pixels[4];
     if(inside) {
      const uint16_t *a=(const uint16_t *)((const uint8_t *)source+iy*stride);
      const uint16_t *b=(const uint16_t *)((const uint8_t *)source+(iy+1)*stride);
      pixels[0]=a[ix];pixels[1]=a[ix+1];pixels[2]=b[ix];pixels[3]=b[ix+1];
     } else {
      uint16_t bg=swapped?swap16(*dst):*dst;
      for(int v=0;v<2;v++) for(int u=0;u<2;u++) {
       int xx=ix+u,yy=iy+v;
       pixels[v*2+u]=(unsigned)xx<360 && (unsigned)yy<140?
         ((const uint16_t *)((const uint8_t *)source+yy*stride))[xx]:bg;
      }
     }
     if(pixels[0]==pixels[1] && pixels[0]==pixels[2] && pixels[0]==pixels[3]) fg=pixels[0];
     else fg=sample565(pixels,wx,wy);
    }
   } else {
    if((unsigned)ix>=360 || (unsigned)iy>=140) continue;
    fg=((const uint16_t *)((const uint8_t *)source+iy*stride))[ix];
   }
   uint16_t bg=swapped?swap16(*dst):*dst;
   uint16_t value=mix>=32?fg:mix565(fg,bg,mix);
   *dst=swapped?swap16(value):value;
  }

 }

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
    !layer->draw_buf || (layer->color_format!=LV_COLOR_FORMAT_RGB565_SWAPPED &&
                        layer->color_format!=LV_COLOR_FORMAT_RGB565))
  return LV_RESULT_INVALID;
 bool swapped=layer->color_format==LV_COLOR_FORMAT_RGB565_SWAPPED;
 lv_area_t area;
 if(!lv_area_intersect(&area,clipped,&t->clip_area) ||
    !lv_area_intersect(&area,&area,&layer->buf_area)) return LV_RESULT_OK;
 int width=lv_area_get_width(&area);
 int height=lv_area_get_height(&area);
 /* A wrapped ribbon angle can hand this loop a clip that never advances.
  * Bail out instead of spinning with interrupts effectively starved. */
 if(width<1 || height<1 || width>480 || height>480) return LV_RESULT_INVALID;
 uint32_t started=k_cycle_get_32();
 /* Write directly into the cached framebuffer; an intermediate scanline
  * would add two PSRAM copies for every transformed bounding rectangle. */

 flux565_job_t job={source,coords,stride,d,layer,area,swapped};
#ifdef ASTER_SYSTEM_ESP_IDF
 if(parallel_rows && !((uintptr_t)layer->draw_buf->data&63) && area.y2-area.y1>=48) {
  int middle=(area.y1+area.y2+1)/2,upper=middle,lower=middle;
  while(upper<area.y2 && (((size_t)(upper-layer->buf_area.y1)*layer->draw_buf->header.stride)&63))upper++;
  while(lower>area.y1 && (((size_t)(lower-layer->buf_area.y1)*layer->draw_buf->header.stride)&63))lower--;
  middle=upper-middle<middle-lower?upper:lower;
  if(middle-area.y1>=16 && area.y2-middle>=16 &&
     !(((size_t)(middle-layer->buf_area.y1)*layer->draw_buf->header.stride)&63))
   parallel_rows(draw565_rows,&job,area.y1,middle,area.y2);
  else draw565_rows(&job,area.y1,area.y2);
 } else
#endif
 draw565_rows(&job,area.y1,area.y2);
 draw_cycles+=(uint32_t)(k_cycle_get_32()-started);return LV_RESULT_OK;
}
