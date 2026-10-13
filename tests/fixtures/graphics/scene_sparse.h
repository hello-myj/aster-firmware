/* Same retained pixels as partial: two distant 8x8 updates per frame. */
#include <aster_gfx_engine.h>
static aster_ge_t ge;
static aster_gfx_cmd_t ring[8];static uint8_t arena[16];
static void bench_setup(uint32_t w,uint32_t h) {
 aster_ge_init(&ge,surface,w,h,ring,8,arena,sizeof(arena));
 aster_ge_clear(&ge,0);if(aster_ge_present(&ge)<0) errors++;
}
static void bench_draw(uint32_t frame_no) {
 uint32_t before=ge.submit_calls;
#ifdef BENCH_TRANSACTION
 if(aster_ge_begin_frame(&ge)<0) {errors++;return;}
#endif
 uint16_t col=(uint16_t)(0xf800u+(frame_no*97u)%0x07ffu);
 uint32_t ax=frame_no%40u,ay=8u+((frame_no/40u)*8u)%(H/2u-16u);
 uint32_t bx=W-16u-(frame_no%40u),by=H/2u+8u+((frame_no/40u)*8u)%(H/2u-16u);
 aster_ge_rect(&ge,ax,ay,8,8,col);aster_ge_rect(&ge,bx,by,8,8,col);
#ifdef BENCH_COMMAND_DAMAGE
 if(aster_ge_present_commands(&ge)<0) errors++;
#else
 if(aster_ge_present(&ge)<0) errors++;
#endif
 gfx_calls+=ge.submit_calls-before;
}
