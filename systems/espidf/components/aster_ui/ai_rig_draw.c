/* SPDX-License-Identifier: MIT */
#include "ai_rig_draw.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

static float lerp(float x, float y, float t) { return x + (y - x) * t; }
static int clamp(int value, int lo, int hi) { return value < lo ? lo : value > hi ? hi : value; }
static unsigned mul_alpha(unsigned a, unsigned b) { return (a * b + 127U) / 255U; }
static inline float smaller(float a, float b) { return a < b ? a : b; }
static inline float larger(float a, float b) { return a > b ? a : b; }
static inline int pixel_boundary(float value)
{
    float edge = value - .5f;
    int whole = (int)edge;
    return whole + (edge > whole);
}

struct rig_paint {
    const struct rig_texture *texture;
    const struct rig_mask_page *mask;
    uint8_t *target_mask;
    unsigned opacity;
    unsigned flags;
    unsigned multiply[3], screen[3];
    bool tint;
};

static inline uint16_t composite(uint16_t back, uint16_t front, unsigned a, const struct rig_paint *paint)
{
    const unsigned masks[] = {31, 63, 31};
    const unsigned shifts[] = {11, 5, 0};
    uint16_t out = 0;
    for (unsigned i = 0; i < 3; i++) {
        int src = (front >> shifts[i]) & masks[i];
        int dst = (back >> shifts[i]) & masks[i];
        if (paint->tint) {
            src = mul_alpha(src, paint->multiply[i]);
            src += mul_alpha(masks[i] - src, paint->screen[i]);
        }
        int value;
        if (paint->flags & csmBlendAdditive)
            value = clamp(dst + (int)mul_alpha(src, a), 0, masks[i]);
        else if (paint->flags & csmBlendMultiplicative)
            value = dst * (255 - a + mul_alpha(src * 255U / masks[i], a)) / 255;
        else
            value = (src * (int)a + dst * (255 - (int)a) + 127) / 255;
        out |= (unsigned)value << shifts[i];
    }
    return out;
}

/* Each scanline uses affine UV gradients and samples at pixel centres. */
static void triangle(struct rig_draw *draw, const struct rig_paint *paint,
                     const struct rig_vertex *a, const struct rig_vertex *b, const struct rig_vertex *c)
{
    if (a->y > b->y) { const struct rig_vertex *v = a; a = b; b = v; }
    if (b->y > c->y) { const struct rig_vertex *v = b; b = c; c = v; }
    if (a->y > b->y) { const struct rig_vertex *v = a; a = b; b = v; }
    int top = pixel_boundary(a->y), bottom = pixel_boundary(c->y);
    if (bottom <= 0 || top >= draw->height || bottom <= top ||
        larger(a->x, larger(b->x, c->x)) < 0 || smaller(a->x, smaller(b->x, c->x)) >= draw->width)
        return;
    const struct rig_mask_page *mask = paint->mask;
    bool inverted = paint->flags & csmIsInvertedMask;
    int ymin = mask && !inverted ? mask->top : 0;
    int ymax = mask && !inverted ? mask->bottom : draw->height;
    int xmin = mask && !inverted ? mask->left : 0;
    int xmax = mask && !inverted ? mask->right : draw->width;
    if (ymin >= ymax || xmin >= xmax)
        return;
    float dy = c->y - a->y, mid = b->y - a->y;
    float dx = c->x - a->x;
    float area = (b->x - a->x) * dy - dx * mid;
    if (fabsf(area) < 0.0001f)
        return;
    const struct rig_texture *tex = paint->texture;
    float du = c->u - a->u, dv = c->v - a->v;
    float inv = 1.0f / area, slope = dx / dy;
    float u_x = ((b->u - a->u) * dy - du * mid) * inv;
    float v_x = ((b->v - a->v) * dy - dv * mid) * inv;
    float u_y = du / dy, v_y = dv / dy;
    /* Allow narrow triangles and protect conversion if the gradient is steep. */
    bool fixed = fabsf(u_x) < 400 && fabsf(v_x) < 400;
    int step_u = fixed ? (int)(u_x * 65536) : 0;
    int step_v = fixed ? (int)(v_x * 65536) : 0;
    int split = pixel_boundary(b->y);
    float upper_slope = mid > 0 ? (b->x - a->x) / mid : 0;
    float lower_slope = c->y > b->y ? (c->x - b->x) / (c->y - b->y) : 0;
    int end_y = clamp(bottom, ymin, ymax);
    for (int y = clamp(top, ymin, ymax); y < end_y; y++) {
        float row = y + 0.5f;
        float edge = a->x + (row - a->y) * slope;
        float other = y < split ? a->x + (row - a->y) * upper_slope : b->x + (row - b->y) * lower_slope;
        int left = clamp(pixel_boundary(smaller(edge, other)), xmin, xmax);
        int right = clamp(pixel_boundary(larger(edge, other)), xmin, xmax);
        if (left >= right)
            continue;
        float offset = left + 0.5f - edge;
        float uf = a->u + u_y * (row - a->y) + u_x * offset;
        float vf = a->v + v_y * (row - a->y) + v_x * offset;
        int u = fixed ? (int)(uf * 65536) : 0, v = fixed ? (int)(vf * 65536) : 0;
        unsigned pixel = y * draw->width + left;
        const uint8_t *coverage = mask && y >= mask->top && y < mask->bottom ? mask->pixels + y * draw->width : NULL;
        for (int x = left; x < right; x++, pixel++, u += step_u, v += step_v) {
            int tx = clamp(fixed ? u >> 16 : (int)(uf + (x - left) * u_x), 0, tex->width - 1);
            int ty = clamp(fixed ? v >> 16 : (int)(vf + (x - left) * v_x), 0, tex->height - 1);
            unsigned sample = ty * tex->width + tx;
            unsigned a4 = (tex->alpha4[sample >> 1] >> ((sample & 1) * 4)) & 15U;
            unsigned alpha = mul_alpha(a4 * 17, paint->opacity);
            if (paint->target_mask) {
                uint8_t *dest = &paint->target_mask[pixel];
                *dest += mul_alpha(255 - *dest, alpha);
                continue;
            }
            if (mask) {
                unsigned covered = coverage && x >= mask->left && x < mask->right ? coverage[x] : 0;
                alpha = mul_alpha(alpha, inverted ? 255 - covered : covered);
            }
            if (!alpha)
                continue;
            uint16_t color = tex->color[sample];
            if (alpha == 255 && !paint->tint && !(paint->flags & (csmBlendAdditive | csmBlendMultiplicative)))
                draw->pixels[pixel] = color;
            else
                draw->pixels[pixel] = composite(draw->pixels[pixel], color, alpha, paint);
        }
    }
}

static void mesh(struct rig_draw *draw, const csmModel *model, int id, const struct rig_mask_page *mask, uint8_t *target)
{
    int texture = csmGetDrawableTextureIndices(model)[id];
    int count = csmGetDrawableVertexCounts(model)[id];
    if (texture < 0 || texture >= RIG_TEXTURES || count <= 0 || count > RIG_MAX_VERTICES)
        return;
    const struct rig_texture *atlas = &draw->textures[texture];
    const csmVector2 *positions = csmGetDrawableVertexPositions(model)[id];
    const csmVector2 *uv = csmGetDrawableVertexUvs(model)[id];
    const csmVector4 *multiply = &csmGetDrawableMultiplyColors(model)[id];
    const csmVector4 *screen = &csmGetDrawableScreenColors(model)[id];
    struct rig_paint paint = {.texture = atlas, .mask = mask, .target_mask = target,
        .opacity = target ? 255 : clamp((int)(255 * csmGetDrawableOpacities(model)[id] + 0.5f), 0, 255),
        .flags = csmGetDrawableConstantFlags(model)[id],
        .multiply = {clamp(multiply->X * 255, 0, 255), clamp(multiply->Y * 255, 0, 255), clamp(multiply->Z * 255, 0, 255)},
        .screen = {clamp(screen->X * 255, 0, 255), clamp(screen->Y * 255, 0, 255), clamp(screen->Z * 255, 0, 255)},
    };
    for (int channel = 0; channel < 3; channel++)
        paint.tint |= paint.multiply[channel] != 255 || paint.screen[channel] != 0;
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    for (int i = 0; i < count; i++) {
        struct rig_vertex *v = &draw->vertices[i];
        *v = (struct rig_vertex){draw->x + positions[i].X * draw->scale,
            draw->y - positions[i].Y * draw->scale, uv[i].X * atlas->width, (1 - uv[i].Y) * atlas->height};
        min_x = smaller(min_x, v->x); max_x = larger(max_x, v->x);
        min_y = smaller(min_y, v->y); max_y = larger(max_y, v->y);
    }
    if (max_x < 0 || min_x >= draw->width || max_y < 0 || min_y >= draw->height)
        return;
    const unsigned short *indices = csmGetDrawableIndices(model)[id];
    int length = csmGetDrawableIndexCounts(model)[id];
    for (int i = 0; i + 2 < length; i += 3)
        triangle(draw, &paint, &draw->vertices[indices[i]], &draw->vertices[indices[i + 1]],
                 &draw->vertices[indices[i + 2]]);
}

static const struct rig_mask_page *clip_mask(struct rig_draw *draw, const csmModel *model, const int *ids, int count)
{
    if (!count)
        return NULL;
    for (unsigned i = 0; i < RIG_MASK_PAGES; i++) {
        if (draw->masks[i].count == count && !memcmp(draw->masks[i].ids, ids, count * sizeof(*ids)))
            return &draw->masks[i];
    }
    struct rig_mask_page *page = &draw->masks[draw->next_mask++ % RIG_MASK_PAGES];
    page->count = count;
    page->ids = ids;
    const int *counts = csmGetDrawableVertexCounts(model);
    const csmVector2 **vertices = csmGetDrawableVertexPositions(model);
    float x1 = draw->width, y1 = draw->height, x2 = 0, y2 = 0;
    for (int i = 0; i < count; i++) {
        int id = ids[i];
        for (int j = 0; j < counts[id]; j++) {
            float x = draw->x + vertices[id][j].X * draw->scale;
            float y = draw->y - vertices[id][j].Y * draw->scale;
            x1 = smaller(x1, x); x2 = larger(x2, x);
            y1 = smaller(y1, y); y2 = larger(y2, y);
        }
    }
    page->left = clamp(pixel_boundary(x1), 0, draw->width);
    page->right = clamp(pixel_boundary(x2), page->left, draw->width);
    page->top = clamp(pixel_boundary(y1), 0, draw->height);
    page->bottom = clamp(pixel_boundary(y2), page->top, draw->height);
    for (int y = page->top; y < page->bottom; y++)
        memset(page->pixels + y * draw->width + page->left, 0, page->right - page->left);
    for (int i = 0; i < count; i++)
        mesh(draw, model, ids[i], NULL, page->pixels);
    return page;
}

void ai_rig_fit(const csmModel *model, struct rig_camera *camera)
{
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    float face_left = 1e9f, face_right = -1e9f, face_top = -1e9f, face_bottom = 1e9f;
    const int *counts = csmGetDrawableVertexCounts(model);
    const csmVector2 **verts = csmGetDrawableVertexPositions(model);
    const int *parents = csmGetDrawableParentPartIndices(model);
    const char **parts = csmGetPartIds(model);
    int meshes = csmGetDrawableCount(model);
    for (int i = 0; i < meshes; i++) {
        bool face = parents[i] >= 0 && (!strcmp(parts[parents[i]], "Part99") ||
                    !strcmp(parts[parents[i]], "Part15") || !strcmp(parts[parents[i]], "Part161"));
        for (int j = 0; j < counts[i]; j++) {
            float x = verts[i][j].X, y = verts[i][j].Y;
            min_x = fminf(min_x, x); max_x = fmaxf(max_x, x);
            min_y = fminf(min_y, y); max_y = fmaxf(max_y, y);
            if (face) {
                face_left = fminf(face_left, x); face_right = fmaxf(face_right, x);
                face_top = fmaxf(face_top, y); face_bottom = fminf(face_bottom, y);
            }
        }
    }
    *camera = (struct rig_camera){(min_x + max_x) / 2, max_y, fmaxf(max_x - min_x, 0.01f),
        fmaxf(max_y - min_y, 0.01f), (min_x + max_x) / 2, lerp(max_y, min_y, 0.19f)};
    if (face_top > face_bottom) {
        camera->face_x = (face_left + face_right) / 2;
        camera->face_y = (face_top + face_bottom) / 2;
    }
}

void ai_rig_draw(struct rig_draw *draw, const csmModel *model, const struct rig_camera *camera,
                 uint16_t *pixels, int zoom)
{
    for (int i = 0; i < draw->width * draw->height; i++)
        pixels[i] = 0x1988;
    draw->pixels = pixels;
    int height = draw->canvas_height ? draw->canvas_height : draw->height;
    float fit = smaller((draw->width - 12) / camera->width, (height - 12) / camera->height);
    float focus = camera->height * .20f;
    draw->scale = fit * zoom / 100.0f;
    float blend = clamp(zoom - 245, 0, 215) / 215.0f;
    draw->x = draw->width * 0.5f - lerp(camera->center_x, camera->face_x, blend) * draw->scale;
    float headroom = height * .09f * clamp(zoom - 100, 0, 60) / 60.0f;
    float anchor = (height - camera->height * fit) * 0.5f + focus * fit + headroom;
    draw->y = lerp(anchor + (camera->top - focus) * draw->scale,
                   height * .55f + camera->face_y * draw->scale, blend) - draw->y_offset;
    for (unsigned i = 0; i < RIG_MASK_PAGES; i++)
        draw->masks[i].count = 0;
    draw->next_mask = 0;
    const int count = csmGetDrawableCount(model);
    const int *order = csmGetDrawableRenderOrders(model);
    const int *mask_counts = csmGetDrawableMaskCounts(model);
    const int **masks = csmGetDrawableMasks(model);
    const char **names = csmGetDrawableIds(model);
    const csmFlags *visible = csmGetDrawableDynamicFlags(model);
    const float *opacity = csmGetDrawableOpacities(model);
    for (int i = 0; i < count; i++)
        draw->order[i] = i;
    for (int i = 1; i < count; i++) {
        unsigned next = draw->order[i];
        int j = i;
        for (; j > 0 && order[draw->order[j - 1]] > order[next]; j--)
            draw->order[j] = draw->order[j - 1];
        draw->order[j] = next;
    }
    for (int i = 0; i < count; i++) {
        int id = draw->order[i];
        if (!(visible[id] & csmIsVisible) || opacity[id] <= 0.001f ||
            !strcmp(names[id], "ArtMesh246") || !strcmp(names[id], "ArtMesh682"))
            continue;
        const struct rig_mask_page *mask = clip_mask(draw, model, masks[id], mask_counts[id]);
        mesh(draw, model, id, mask, NULL);
    }
}
