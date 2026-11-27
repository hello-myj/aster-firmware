/* SPDX-License-Identifier: MIT */
#pragma once
#include "PurismCore.h"
#include <stdbool.h>
#include <stdint.h>

enum { RIG_TEXTURES = 5, RIG_MASK_PAGES = 3, RIG_MAX_MESHES = 1024, RIG_MAX_VERTICES = 2048 };
enum { RIG_WIDTH = 480, RIG_HEIGHT = 480, RIG_TILE_SIZE = 32, RIG_TILE_COLS = RIG_WIDTH / RIG_TILE_SIZE,
       RIG_TILE_ROWS = RIG_HEIGHT / RIG_TILE_SIZE, RIG_TILE_MASK = (1 << RIG_TILE_COLS) - 1 };

struct rig_area { int16_t x1, y1, x2, y2; }; /* Right and bottom are exclusive. */
struct rig_tiles { uint16_t rows[RIG_TILE_ROWS]; };

struct rig_mesh_snapshot {
    struct rig_area area;
    unsigned vertex_offset;
    int order;
    uint32_t multiply, screen;
    uint8_t opacity;
    bool visible, mask_source;
};

struct rig_damage {
    struct rig_mesh_snapshot *meshes;
    csmVector2 *vertices;
    int zoom;
};

struct rig_texture {
    unsigned width, height;
    const uint16_t *color;
    const uint8_t *alpha4;
};

struct rig_vertex { float x, y, u, v; };

struct rig_camera {
    float center_x, top, width, height, face_x, face_y;
};

struct rig_mask_page {
    uint8_t *pixels;
    const int *ids;
    int count;
    int left, top, right, bottom;
};

struct rig_draw {
    struct rig_texture textures[RIG_TEXTURES];
    struct rig_vertex vertices[RIG_MAX_VERTICES];
    struct rig_mask_page masks[RIG_MASK_PAGES];
    uint16_t order[RIG_MAX_MESHES];
    uint16_t *pixels;
    int width, height, canvas_height, y_offset;
    float scale, x, y;
    int next_mask;
    const struct rig_tiles *tiles;
    const struct rig_mesh_snapshot *meshes;
};

void ai_rig_fit(const csmModel *model, struct rig_camera *camera);
unsigned ai_rig_collect_damage(struct rig_damage *damage, const csmModel *model,
                               const struct rig_camera *camera, int zoom, struct rig_tiles *tiles);
/* Consume one contiguous horizontal span. Caller skips rows whose mask is zero. */
static inline unsigned ai_rig_take_span(unsigned *bits, unsigned *first)
{
    *first = (unsigned)__builtin_ctz(*bits);
    unsigned last = (unsigned)__builtin_ctz(~(*bits >> *first)) + *first;
    *bits &= ~((1U << last) - 1U);
    return last;
}
void ai_rig_draw(struct rig_draw *draw, const csmModel *model, const struct rig_camera *camera,
                 uint16_t *pixels, int zoom);
