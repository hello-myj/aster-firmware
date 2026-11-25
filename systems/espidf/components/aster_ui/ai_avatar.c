/* SPDX-License-Identifier: MIT */
#include "ai_avatar.h"
#include "avatar_asset.h"
#include "aster_os.h"
#include "aster_services.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#include "vendor/miniz.h"
#include <string.h>

enum { CANVAS = 460, MAX_CLIPS = 64, MAX_FRAMES = 2048, MAX_PACK_BYTES = 32 * 1024 * 1024 };

/* Little-endian AWA1 layout shared with tools/pack_ai_avatar.py. */
struct avatar_header {
    char magic[4];
    uint16_t canvas_width, canvas_height, x, y, width, height;
    uint32_t fps, clips, frames, max_compressed;
};
struct avatar_clip {
    uint32_t first;
    uint16_t frames, loop;
    char label[48];
};
_Static_assert(sizeof(struct avatar_header) == 32, "AWA1 header");
_Static_assert(sizeof(struct avatar_clip) == 56, "AWA1 clip record");

struct avatar {
    struct avatar_header header;
    struct avatar_clip *clips;
    uint32_t *offsets;
    uint8_t *pixels, *compressed;
    size_t pixels_bytes;
    aster_os_file_t file;
    lv_image_dsc_t descriptor;
    lv_obj_t *image;
    lv_timer_t *timer;
    tinfl_decompressor decoder;
    uint32_t clip, frame, started, updates;
    bool animate;
};

static bool read_exact(aster_os_file_t *file, void *destination, size_t bytes)
{
    uint8_t *p = destination;
    while (bytes) {
        int n = aster_os_file_read(file, p, bytes);
        if (n <= 0 || (size_t)n > bytes)
            return false;
        p += n;
        bytes -= n;
    }
    return true;
}

static bool load_pack(struct avatar *a, uint64_t file_bytes)
{
    struct avatar_header *h = &a->header;
    if (!read_exact(&a->file, h, sizeof(*h)) || memcmp(h->magic, "AWA1", 4) ||
        h->canvas_width != CANVAS || h->canvas_height != CANVAS ||
        !h->width || !h->height || h->x + h->width > CANVAS || h->y + h->height > CANVAS ||
        h->fps != 15 || !h->clips || h->clips > MAX_CLIPS ||
        !h->frames || h->frames > MAX_FRAMES)
        return false;
    a->pixels_bytes = (size_t)h->width * h->height * 3;
    if (!h->max_compressed || h->max_compressed > a->pixels_bytes + 1024)
        return false;
    size_t clips_bytes = h->clips * sizeof(*a->clips);
    size_t index_bytes = (h->frames + 1) * sizeof(*a->offsets);
    size_t data_start = sizeof(*h) + clips_bytes + index_bytes;
    if (file_bytes <= data_start)
        return false;
    a->clips = aster_os_alloc_external(clips_bytes);
    a->offsets = aster_os_alloc_external(index_bytes);
    a->pixels = aster_os_alloc_external(a->pixels_bytes);
    a->compressed = aster_os_alloc_external(h->max_compressed);
    if (!a->clips || !a->offsets || !a->pixels || !a->compressed ||
        !read_exact(&a->file, a->clips, clips_bytes) || !read_exact(&a->file, a->offsets, index_bytes))
        return false;
    uint32_t frames = 0;
    for (unsigned i = 0; i < h->clips; i++) {
        const struct avatar_clip *clip = &a->clips[i];
        if (clip->first != frames || !clip->frames || clip->frames > h->frames - frames ||
            clip->loop > 1 || !clip->label[0] || !memchr(clip->label, 0, sizeof(clip->label)))
            return false;
        frames += clip->frames;
    }
    if (frames != h->frames || a->offsets[0] != data_start || a->offsets[frames] != file_bytes)
        return false;
    for (unsigned i = 0; i < frames; i++)
        if (a->offsets[i + 1] <= a->offsets[i] ||
            a->offsets[i + 1] - a->offsets[i] > h->max_compressed)
            return false;
    return true;
}

static void release_pack(struct avatar *a)
{
    if (a->file.handle)
        aster_os_file_close(&a->file);
    aster_os_free_external(a->compressed);
    aster_os_free_external(a->pixels);
    aster_os_free_external(a->offsets);
    aster_os_free_external(a->clips);
    aster_os_free_external(a);
}

static bool decode_frame(struct avatar *a, unsigned frame)
{
    if (frame == a->frame)
        return true;
    size_t input = a->offsets[frame + 1] - a->offsets[frame];
    size_t output = a->pixels_bytes, expected = input;
    if (aster_os_file_seek(&a->file, a->offsets[frame], ASTER_OS_SEEK_SET) ||
        !read_exact(&a->file, a->compressed, input))
        return false;
    tinfl_init(&a->decoder);
    tinfl_status status = tinfl_decompress(&a->decoder, a->compressed, &input, a->pixels,
        a->pixels, &output, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE || input != expected || output != a->pixels_bytes)
        return false;
    a->frame = frame;
    if (a->image) {
        lv_image_cache_drop(&a->descriptor);
        lv_obj_invalidate(a->image);
        if (++a->updates == 30)
            aster_service_logf(3, "ai-avatar: 30 frames in %lu ms",
                               (unsigned long)(aster_os_uptime32() - a->started));
    }
    return true;
}

static void stop_on_error(struct avatar *a)
{
    lv_timer_pause(a->timer);
    lv_obj_add_flag(a->image, LV_OBJ_FLAG_HIDDEN);
    aster_service_logf(2, "ai-avatar: frame could not be decoded");
}

static void play_frame(lv_timer_t *timer)
{
    struct avatar *a = lv_timer_get_user_data(timer);
    const struct avatar_clip *clip = &a->clips[a->clip];
    uint64_t elapsed = (uint32_t)(aster_os_uptime32() - a->started);
    uint32_t fps = a->header.fps;
    uint64_t sequence = elapsed * fps / 1000;
    uint32_t index = (uint32_t)sequence;
    /* Schedule against the source clock so SD reads and the UI tick's
     * rounding do not add a delay to every subsequent frame. */
    uint64_t next = ((sequence + 1) * 1000 + fps - 1) / fps;
    lv_timer_set_period(timer, (uint32_t)(next - elapsed));
    if (clip->loop)
        index %= clip->frames;
    else if (index >= (uint32_t)clip->frames - 1) {
        index = clip->frames - 1;
        lv_timer_pause(timer);
    }
    if (!decode_frame(a, clip->first + index))
        stop_on_error(a);
}

static void delete_image(lv_event_t *event)
{
    struct avatar *a = lv_event_get_user_data(event);
    lv_timer_delete(a->timer);
    lv_image_cache_drop(&a->descriptor);
    release_pack(a);
}

lv_obj_t *ai_avatar_create(lv_obj_t *parent)
{
    aster_os_entry_t file;
    if (aster_os_stat(ASTER_AI_AVATAR_PATH, &file) || file.type != ASTER_OS_FILE ||
        file.size > MAX_PACK_BYTES)
        return NULL;
    struct avatar *a = aster_os_alloc_external(sizeof(*a));
    if (!a)
        return NULL;
    memset(a, 0, sizeof(*a));
    a->frame = UINT32_MAX;
    a->clip = UINT32_MAX;
    if (aster_os_file_open(&a->file, ASTER_AI_AVATAR_PATH, ASTER_OS_READ) ||
        !load_pack(a, file.size) || !decode_frame(a, 0)) {
        release_pack(a);
        aster_service_logf(2, "ai-avatar: invalid SD asset");
        return NULL;
    }
    a->timer = lv_timer_create(play_frame, 1000 / a->header.fps, a);
    if (!a->timer) {
        release_pack(a);
        return NULL;
    }
    lv_timer_pause(a->timer);
    a->descriptor.header = (lv_image_header_t){
        .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8,
        .w = a->header.width, .h = a->header.height, .stride = a->header.width * 2,
    };
    a->descriptor.data_size = a->pixels_bytes;
    a->descriptor.data = a->pixels;
    a->image = lv_image_create(parent);
    if (!a->image) {
        lv_timer_delete(a->timer);
        release_pack(a);
        return NULL;
    }
    lv_obj_set_user_data(a->image, a);
    lv_obj_add_event_cb(a->image, delete_image, LV_EVENT_DELETE, a);
    lv_image_set_src(a->image, &a->descriptor);
    lv_obj_set_pos(a->image, a->header.x, a->header.y);
    aster_service_logf(3, "ai-avatar: %ux%u RGB565, %lu clips, %lu FPS",
                       a->header.canvas_width, a->header.canvas_height,
                       (unsigned long)a->header.clips, (unsigned long)a->header.fps);
    return a->image;
}

void ai_avatar_play(lv_obj_t *image, const char *label, bool animate)
{
    struct avatar *a = lv_obj_get_user_data(image);
    unsigned clip = 0;
    for (unsigned i = 0; i < a->header.clips; i++)
        if (!strcmp(a->clips[i].label, label)) {
            clip = i;
            break;
        }
    if (clip == a->clip && animate == a->animate && !lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN))
        return;
    lv_timer_pause(a->timer);
    a->clip = clip;
    a->animate = animate;
    a->started = aster_os_uptime32();
    a->updates = 0;
    if (!decode_frame(a, a->clips[clip].first)) {
        stop_on_error(a);
        return;
    }
    lv_obj_clear_flag(image, LV_OBJ_FLAG_HIDDEN);
    aster_service_logf(3, "ai-avatar: pose=%s", a->clips[clip].label);
    if (animate && a->clips[clip].frames > 1) {
        lv_timer_set_period(a->timer, (1000 + a->header.fps - 1) / a->header.fps);
        lv_timer_reset(a->timer);
        lv_timer_resume(a->timer);
    }
}
