/* Twelve separated patches exercise 8/16-region capacity on identical pixels. */
#define BENCH_NAME "dirtygrid"
#include "../../bench.h"
#include <aster_gfx_engine.h>
static aster_ge_t ge;static aster_gfx_cmd_t ring[24];static uint8_t arena[16];
static void bench_setup(uint32_t w,uint32_t h) {
 aster_ge_init(&ge,surface,w,h,ring,24,arena,sizeof(arena));
 aster_ge_clear(&ge,0);if(aster_ge_present(&ge)<0)errors++;
}
static void bench_draw(uint32_t frame_no) {
 uint32_t before=ge.submit_calls;
 for(uint32_t i=0;i<12;i++) {
  uint32_t x=(i%4u)*(W-8u)/3u,y=(i/4u)*(H-8u)/2u;
  aster_ge_rect(&ge,x,y,8,8,(uint16_t)(0xf800u+(frame_no*97u+i*16u)%0x07ffu));
 }
 if(aster_ge_present_commands(&ge)<0)errors++;
 gfx_calls+=ge.submit_calls-before;
}
