/* 128-frame fixed sprite scene: opaque, alpha, scaled and color-key edges on
 * two loaded images (a gradient with a black square hole, and an all-black
 * tile). This is the device half of G2/G3: the same handles drawn every frame
 * must stay the same picture (no cross-image handle reuse), and the color key
 * 0x0000 must punch through to the clear color, not paint black. Loaded from
 * the app's own /assets (needs the fs permission). */
#define BENCH_NAME "image"
#include "../../bench.h"
#include <aster_gfx.h>
#include <aster_gfx_engine.h>
static uint32_t h_color, h_black, cw, chh, bw, bh;
static aster_ge_t ge;
static aster_gfx_cmd_t ring[64];
static uint8_t arena[128];
static void bench_setup(uint32_t width, uint32_t height) { (void)width;(void)height;
 aster_ge_init(&ge,surface,W,H,ring,sizeof(ring)/sizeof(ring[0]),arena,sizeof(arena));
 if(aster_gfx_bmp_load("/assets/assets/color.bmp",24u,64,64,&h_color,&cw,&chh)!=ASTER_OK)errors++;
 if(aster_gfx_bmp_load("/assets/assets/black.bmp",24u,32,32,&h_black,&bw,&bh)!=ASTER_OK)errors++;
 if(!h_color||!h_black)errors++;
}
static void bench_draw(uint32_t frame_no) {
 uint32_t calls_before=ge.submit_calls;
 aster_ge_clear(&ge,0x0841);
 /* opaque 1:1 at a fixed spot; the black square in color.bmp must fall back
  * to the clear color through the 0x0000 color key. */
 aster_ge_bmp(&ge,h_color,cw,chh,8,8,256u,255u);
 /* alpha pulse keeps frames distinct yet identical between runs */
 uint16_t alpha=(uint16_t)(96u+(frame_no*3u)%160u);
 aster_ge_bmp(&ge,h_color,cw,chh,72,8,256u,alpha);
 /* 2x nearest scale */
 aster_ge_bmp(&ge,h_black,bw,bh,8,80,512u,255u);
 /* scaled + alpha; all-black content is fully color-keyed -> shows clear */
 aster_ge_bmp(&ge,h_black,bw,bh,80,80,384u,alpha);
 int32_t r=aster_ge_present(&ge);if(r<0)errors++;gfx_calls+=ge.submit_calls-calls_before;
 /* release/reload the same path on a boundary frame: the freed handle must
  * never alias a still-valid one, and the redrawn frame must match. */
 if(frame_no==63){aster_gfx_bmp_free(h_color);
  if(aster_gfx_bmp_load("/assets/assets/color.bmp",24u,64,64,&h_color,&cw,&chh)!=ASTER_OK)errors++;}
}
