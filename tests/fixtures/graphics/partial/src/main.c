/* 128-frame partial-update scene: two small 8x8 blocks far apart, pushed as
 * TWO explicit dirty rects per frame (no full-screen clear after frame 0).
 * G0's transport question: how many panel bytes does the glass actually get
 * per logic frame? The union-of-dirty shortcut (one huge rect) versus two
 * bounded sends is what the G5 measurement will compare against this.
 * Pixels are hand-painted through the acquired buffer, then each region is
 * pushed via aster_surface_flush: one rect per call, twice per frame. */
#define BENCH_NAME "partial"
#include "../../bench.h"
static uint16_t *pixels;
static uint32_t pitch; /* in pixels */
static void bench_setup(uint32_t width, uint32_t height) { (void)width;(void)height;
 aster_surface_buffer_t b={.size=sizeof(b),.version=1};
 if(aster_surface_acquire(surface,&b,sizeof(b))!=ASTER_OK){errors++;return;}
 pixels=(uint16_t*)b.ptr;pitch=b.pitch/2u;
 /* opaque black base so later partial paints are the only changes */
 for(uint32_t y=0;y<H;y++)for(uint32_t x=0;x<W;x++)pixels[y*pitch+x]=0x0000;
 if(aster_surface_flush(surface,pixels,0,0,W,H,pitch)<0)errors++;else gfx_calls++;
 /* Register the retained guest surface for the screenshot fallback without
  * sending pixels. ST7789 does not retain a downstream full-panel record
  * across partial writes. This capture checks guest pixels, not wire order. */
 if(aster_gfx_exec(surface,0,0,0,0,0,0,0,0)<0)errors++;

}
static void bench_draw(uint32_t frame_no) {
 /* block A top-left region, block B bottom-right region: 8px squares, hue
  * cycles per frame; only their pixels are written, then each ships alone. */
 uint16_t col=(uint16_t)(0xf800u+(frame_no*97u)%0x07ffu);
 uint32_t ax=(frame_no%40u), ay=8u+((frame_no/40u)*8u)%(H/2u-16u);
 uint32_t bx=W-16u-(frame_no%40u), by=H/2u+8u+((frame_no/40u)*8u)%(H/2u-16u);
 for(uint32_t j=0;j<8u;j++){for(uint32_t i=0;i<8u;i++){
  pixels[(ay+j)*pitch+ax+i]=col; pixels[(by+j)*pitch+bx+i]=col;}}
 /* two bounded sends: the actual panel work for two far-apart updates */
 if(aster_surface_flush(surface,pixels+ay*pitch+ax,ax,ay,8,8,pitch)<0)errors++;else gfx_calls++;
 if(aster_surface_flush(surface,pixels+by*pitch+bx,bx,by,8,8,pitch)<0)errors++;else gfx_calls++;
}
