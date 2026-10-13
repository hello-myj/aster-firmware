/* Same scene as `text`, but ring capacity 3 and arena 24 B: almost every
 * command spills. Before the G1 joint-reservation fix this lost the text
 * after a mid-frame spill (the handbook repro); now it must end on the same
 * final frame as `text`, pixel for pixel. */
#define BENCH_NAME "smallring"
#include "../../bench.h"
#include <aster_gfx.h>
#include <aster_gfx_engine.h>
static aster_gfx_cmd_t tiny_ring[3];
static uint8_t tiny_arena[24];
#define RING tiny_ring
#define RING_N 3u
#define ARENA tiny_arena
#define ARENA_CAP 24u
#include "../../scene_text.h"
static void bench_setup(uint32_t width, uint32_t height) { (void)width;(void)height;scene_setup(); }
static void bench_draw(uint32_t frame_no) { scene_draw(frame_no); }
