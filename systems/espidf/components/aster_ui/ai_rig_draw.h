/* SPDX-License-Identifier: MIT */
#pragma once
#include "PurismCore.h"
#include <stdint.h>

enum { RIG_TEXTURES = 5, RIG_MASK_PAGES = 3, RIG_MAX_MESHES = 1024, RIG_MAX_VERTICES = 2048 };

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
};

void ai_rig_fit(const csmModel *model, struct rig_camera *camera);
void ai_rig_draw(struct rig_draw *draw, const csmModel *model, const struct rig_camera *camera,
                 uint16_t *pixels, int zoom);
