/* 128-frame fixed text + marching rects on roomy ring/arena (scene_text.h).
 * The device half of the G1 contract: with `smallring` (the same scene on
 * ring 3 / arena 24 B), the final published frame must be pixel-identical. */
#define BENCH_NAME "text"
#include "../../bench.h"
#include <aster_gfx.h>
#include <aster_gfx_engine.h>
static aster_gfx_cmd_t big_ring[160];
static uint8_t big_arena[1024];
#define RING big_ring
#define RING_N 160u
#define ARENA big_arena
#define ARENA_CAP 1024u
#include "../../scene_text.h"
static void bench_setup(uint32_t width, uint32_t height) { (void)width;(void)height;scene_setup(); }
static void bench_draw(uint32_t frame_no) { scene_draw(frame_no); }
