/* Small batches, one negotiated frame publication. Pixel-equivalent to text. */
#define BENCH_NAME "frame"
#define BENCH_TRANSACTION 1
#include "../../bench.h"
#include <aster_gfx_engine.h>
static aster_gfx_cmd_t commands[3];static uint8_t bytes[24];
#define RING commands
#define RING_N 3u
#define ARENA bytes
#define ARENA_CAP 24u
#include "../../scene_text.h"
static void bench_setup(uint32_t w,uint32_t h){(void)w;(void)h;scene_setup();}
static void bench_draw(uint32_t n){scene_draw(n);}
