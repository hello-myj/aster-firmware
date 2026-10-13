/* Shared text scene for the `text` and `smallring` modes. Both must render
 * pixel-identical final frames: the device-side proof that ring/arena
 * capacity changes nothing about what reaches the glass (the G1
 * joint-reservation contract), while `smallring` (ring 3, arena 24 B) forces
 * a mid-frame spill around almost every command and `text` (ring 160, arena
 * 1 KiB) never spills. Included AFTER bench.h; the mode supplies RING/RING_N
 * and ARENA/ARENA_CAP. */
static aster_ge_t ge;
static void scene_setup(void) {
 aster_ge_init(&ge,surface,W,H,RING,RING_N,ARENA,ARENA_CAP);
}
static void scene_lines(void) {
 /* CJK/Latin mix with descenders; longest run 22 B (fits a 24 B arena). */
 aster_ge_text(&ge,4,20,16,0xffff,ASTER_GFX_NOBG,"得分 42 GFX");
 aster_ge_text(&ge,4,44,16,0x07e0,ASTER_GFX_NOBG,"图形引擎 混排 OK");
 aster_ge_text(&ge,4,68,16,0xf81f,ASTER_GFX_NOBG,"jp g q y p");
 aster_ge_text(&ge,4,92,16,0x001f,ASTER_GFX_NOBG,"得分 100 温度");
 aster_ge_text(&ge,4,116,16,0xffe0,ASTER_GFX_NOBG,"The quick brown");
 aster_ge_text(&ge,4,140,16,0xf800,ASTER_GFX_NOBG,"混排测试行六");
 aster_ge_text(&ge,4,164,16,0x087f,ASTER_GFX_NOBG,"fps=60 frames=128");
 aster_ge_text(&ge,4,188,16,0x8431,ASTER_GFX_NOBG,"中文与 English");
 aster_ge_text(&ge,4,212,16,0x210a,ASTER_GFX_NOBG,"arena row nine");
 aster_ge_text(&ge,4,236,16,0x528a,ASTER_GFX_NOBG,"最后一行 tail");
}
static void scene_draw(uint32_t frame_no) {
 uint32_t calls_before=ge.submit_calls;
#ifdef BENCH_TRANSACTION
 if(aster_ge_begin_frame(&ge)<0){errors++;return;}
#endif
 aster_ge_clear(&ge,0x0841);
 /* deterministic marching rects: frames differ from each other but run the
  * same sequence on both ring sizes; nothing depends on capacity. */
 for (uint32_t i=0;i<8;i++) {
  uint32_t x=(frame_no*2u+i*29u)%(W-24u), y=(frame_no+i*23u)%(H/2u-16u);
  aster_ge_rect(&ge,(int32_t)x,(int32_t)y,24,10,(uint16_t)(0xf800u+i*0x0800u));
 }
 scene_lines();
 int32_t r=aster_ge_present(&ge);if(r<0)errors++;gfx_calls+=ge.submit_calls-calls_before;
}
