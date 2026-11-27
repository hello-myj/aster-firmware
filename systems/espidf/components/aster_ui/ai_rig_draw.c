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

struct rig_projection { float scale, x, y; };

static struct rig_projection project(const struct rig_camera *camera, int width, int height, int zoom)
{
    float fit = smaller((width - 12) / camera->width, (height - 12) / camera->height);
    float focus = camera->height * .20f;
    float scale = fit * zoom / 100.0f;
    float blend = clamp(zoom - 245, 0, 215) / 215.0f;
    float x = width * .5f - lerp(camera->center_x, camera->face_x, blend) * scale;
    float headroom = height * .09f * clamp(zoom - 100, 0, 60) / 60.0f;
    float anchor = (height - camera->height * fit) * .5f + focus * fit + headroom;
    float y = lerp(anchor + (camera->top - focus) * scale, height * .55f + camera->face_y * scale, blend);
    return (struct rig_projection){scale, x, y};
}

static bool render_visible(const char *name, csmFlags flags, float opacity)
{
    return (flags & csmIsVisible) && opacity > .001f &&
           strcmp(name, "ArtMesh246") && strcmp(name, "ArtMesh682");
}

static uint32_t tint_rgb(const csmVector4 *color)
{
    unsigned r = clamp(color->X * 255, 0, 255);
    unsigned g = clamp(color->Y * 255, 0, 255);
    unsigned b = clamp(color->Z * 255, 0, 255);
    return r << 16 | g << 8 | b;
}

static unsigned area_columns(const struct rig_area *area)
{
    unsigned first = area->x1 / RIG_TILE_SIZE;
    unsigned last = (area->x2 + RIG_TILE_SIZE - 1) / RIG_TILE_SIZE;
    return ((1U << last) - 1U) ^ ((1U << first) - 1U);
}

static void mark_area(struct rig_tiles *tiles, const struct rig_area *area)
{
    if (area->x2 <= area->x1 || area->y2 <= area->y1)
        return;
    unsigned columns = area_columns(area);
    for (int row = area->y1 / RIG_TILE_SIZE; row <= (area->y2 - 1) / RIG_TILE_SIZE; row++)
        tiles->rows[row] |= columns;
}

static bool intersects_tiles(const struct rig_draw *draw, const struct rig_area *area)
{
    int top = clamp(area->y1, draw->y_offset, draw->y_offset + draw->height);
    int bottom = clamp(area->y2, top, draw->y_offset + draw->height);
    if (bottom <= top || area->x2 <= area->x1)
        return false;
    unsigned columns = area_columns(area);
    for (int row = top / RIG_TILE_SIZE; row <= (bottom - 1) / RIG_TILE_SIZE; row++)
        if (draw->tiles->rows[row] & columns)
            return true;
    return false;
}

static struct rig_area mesh_area(const csmVector2 *vertices, int count, struct rig_projection p)
{
    if (count <= 0)
        return (struct rig_area){0};
    float left = vertices[0].X, right = left, top = vertices[0].Y, bottom = top;
    for (int i = 1; i < count; i++) {
        left = smaller(left, vertices[i].X); right = larger(right, vertices[i].X);
        bottom = smaller(bottom, vertices[i].Y); top = larger(top, vertices[i].Y);
    }
    /* A narrow guard covers rounding at the two independent band origins. */
    int x1 = clamp(pixel_boundary(p.x + left * p.scale) - 1, 0, RIG_WIDTH);
    int y1 = clamp(pixel_boundary(p.y - top * p.scale) - 1, 0, RIG_HEIGHT);
    int x2 = clamp(pixel_boundary(p.x + right * p.scale) + 1, x1, RIG_WIDTH);
    int y2 = clamp(pixel_boundary(p.y - bottom * p.scale) + 1, y1, RIG_HEIGHT);
    return (struct rig_area){x1, y1, x2, y2};
}

unsigned ai_rig_collect_damage(struct rig_damage *damage, const csmModel *model,
                               const struct rig_camera *camera, int zoom, struct rig_tiles *tiles)
{
    memset(tiles, 0, sizeof(*tiles));
    bool full = !damage->vertices || damage->zoom != zoom;
    if (full) {
        for (int row = 0; row < RIG_TILE_ROWS; row++)
            tiles->rows[row] = RIG_TILE_MASK;
        if (!damage->vertices)
            return RIG_TILE_ROWS * RIG_TILE_COLS;
    }
    damage->zoom = zoom;
    int count = csmGetDrawableCount(model);
    const int *counts = csmGetDrawableVertexCounts(model);
    const int *order = csmGetDrawableRenderOrders(model);
    const int *mask_counts = csmGetDrawableMaskCounts(model);
    const int **masks = csmGetDrawableMasks(model);
    const csmVector2 **vertices = csmGetDrawableVertexPositions(model);
    const char **names = csmGetDrawableIds(model);
    const csmFlags *flags = csmGetDrawableDynamicFlags(model);
    const float *opacity = csmGetDrawableOpacities(model);
    const csmVector4 *multiply = csmGetDrawableMultiplyColors(model);
    const csmVector4 *screen = csmGetDrawableScreenColors(model);
    bool visible[RIG_MAX_MESHES], used_mask[RIG_MAX_MESHES] = {0};
    for (int id = 0; id < count; id++) {
        visible[id] = render_visible(names[id], flags[id], opacity[id]);
        if (visible[id]) {
            for (int j = 0; j < mask_counts[id]; j++)
                used_mask[masks[id][j]] = true;
        }
    }
    struct rig_projection projection = project(camera, RIG_WIDTH, RIG_HEIGHT, zoom);
    for (int id = 0; id < count; id++) {
        struct rig_mesh_snapshot *old = &damage->meshes[id];
        bool was_active = old->visible || old->mask_source;
        bool active = visible[id] || used_mask[id];
        if (!was_active && !active)
            continue;
        size_t bytes = counts[id] * sizeof(csmVector2);
        csmVector2 *previous = damage->vertices + old->vertex_offset;
        bool moved = active && (full || !was_active || memcmp(previous, vertices[id], bytes));
        unsigned alpha = clamp((int)(255 * opacity[id] + .5f), 0, 255);
        uint32_t mul = tint_rgb(&multiply[id]), scr = tint_rgb(&screen[id]);
        bool changed = moved || old->visible != visible[id] || old->mask_source != used_mask[id] ||
                       (visible[id] && (old->order != order[id] || old->opacity != alpha ||
                                        old->multiply != mul || old->screen != scr));
        if (!changed)
            continue;
        if (!full && was_active)
            mark_area(tiles, &old->area);
        if (moved) {
            old->area = mesh_area(vertices[id], counts[id], projection);
            memcpy(previous, vertices[id], bytes);
        }
        if (!full && active)
            mark_area(tiles, &old->area);
        old->visible = visible[id];
        old->mask_source = used_mask[id];
        old->order = order[id];
        old->opacity = alpha;
        old->multiply = mul;
        old->screen = scr;
    }
    unsigned dirty = 0;
    for (int row = 0; row < RIG_TILE_ROWS; row++)
        dirty += __builtin_popcount(tiles->rows[row]);
    return dirty;
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

static inline unsigned row_tiles(const struct rig_draw *draw, int y)
{
    return draw->tiles ? draw->tiles->rows[(y + draw->y_offset) / RIG_TILE_SIZE] : RIG_TILE_MASK;
}

static void clear_mask_row(const struct rig_draw *draw, uint8_t *row, int y, int left, int right)
{
    unsigned bits = row_tiles(draw, y);
    if (bits == RIG_TILE_MASK) {
        memset(row + left, 0, right - left);
        return;
    }
    while (bits) {
        unsigned first;
        unsigned last = ai_rig_take_span(&bits, &first);
        int start = clamp(first * RIG_TILE_SIZE, left, right);
        int end = clamp(last * RIG_TILE_SIZE, start, right);
        memset(row + start, 0, end - start);
    }
}

static void paint_span(struct rig_draw *draw, const struct rig_paint *paint, int y, int first, int last,
                       int origin, float uf, float vf, float u_x, float v_x, bool fixed, int step_u, int step_v)
{
    const struct rig_mask_page *mask = paint->mask;
    const struct rig_texture *tex = paint->texture;
    bool inverted = paint->flags & csmIsInvertedMask;
    /* UV rounding uses the original unclipped span origin, including when
     * the pixels are split over several dirty tile runs. */
    int u = fixed ? (int)(uf * 65536) + (first - origin) * step_u : 0;
    int v = fixed ? (int)(vf * 65536) + (first - origin) * step_v : 0;
    unsigned pixel = y * draw->width + first;
    const uint8_t *coverage = mask && y >= mask->top && y < mask->bottom ? mask->pixels + y * draw->width : NULL;
    for (int x = first; x < last; x++, pixel++, u += step_u, v += step_v) {
        int tx = clamp(fixed ? u >> 16 : (int)(uf + (x - origin) * u_x), 0, tex->width - 1);
        int ty = clamp(fixed ? v >> 16 : (int)(vf + (x - origin) * v_x), 0, tex->height - 1);
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
        unsigned bits = row_tiles(draw, y);
        if (!bits)
            continue;
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
        if (bits == RIG_TILE_MASK) {
            paint_span(draw, paint, y, left, right, left, uf, vf, u_x, v_x, fixed, step_u, step_v);
            continue;
        }
        struct rig_area span = {.x1 = left, .x2 = right};
        bits &= area_columns(&span);
        while (bits) {
            unsigned first;
            unsigned last = ai_rig_take_span(&bits, &first);
            int start = clamp(first * RIG_TILE_SIZE, left, right);
            int end = clamp(last * RIG_TILE_SIZE, start, right);
            paint_span(draw, paint, y, start, end, left, uf, vf, u_x, v_x, fixed, step_u, step_v);
        }
    }
}

static void mesh(struct rig_draw *draw, const csmModel *model, int id, const struct rig_mask_page *mask, uint8_t *target)
{
    int texture = csmGetDrawableTextureIndices(model)[id];
    int count = csmGetDrawableVertexCounts(model)[id];
    if (texture < 0 || texture >= RIG_TEXTURES || count <= 0 || count > RIG_MAX_VERTICES)
        return;
    if (target && draw->meshes && !intersects_tiles(draw, &draw->meshes[id].area))
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
        clear_mask_row(draw, page->pixels + y * draw->width, y, page->left, page->right);
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
    draw->pixels = pixels;
    int height = draw->canvas_height ? draw->canvas_height : draw->height;
    struct rig_projection projection = project(camera, draw->width, height, zoom);
    draw->scale = projection.scale;
    draw->x = projection.x;
    draw->y = projection.y - draw->y_offset;
    bool active = false;
    for (int y = 0; y < draw->height; y++) {
        unsigned bits = row_tiles(draw, y);
        active |= bits != 0;
        if (bits == RIG_TILE_MASK) {
            for (int x = 0; x < draw->width; x++)
                pixels[y * draw->width + x] = 0x1988;
            continue;
        }
        while (bits) {
            unsigned first;
            unsigned last = ai_rig_take_span(&bits, &first);
            for (unsigned x = first * RIG_TILE_SIZE; x < last * RIG_TILE_SIZE; x++)
                pixels[y * draw->width + x] = 0x1988;
        }
    }
    if (!active)
        return;
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
        if (!render_visible(names[id], visible[id], opacity[id]) ||
            (draw->meshes && !intersects_tiles(draw, &draw->meshes[id].area)))
            continue;
        const struct rig_mask_page *mask = clip_mask(draw, model, masks[id], mask_counts[id]);
        mesh(draw, model, id, mask, NULL);
    }
}
