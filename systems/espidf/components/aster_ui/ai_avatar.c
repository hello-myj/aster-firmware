/* SPDX-License-Identifier: MIT */
#include "ai_avatar.h"
#include "avatar_asset.h"
#include "aster_os.h"
#include "aster_services.h"
#include "aster_idf_ui.h"
#include "ai_dialog.h"
#include "ai_reply_audio.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "esp_timer.h"
#ifdef ASTER_AI_AVATAR_P4_ACCEL
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#endif
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#include "vendor/miniz.h"
#include <string.h>

enum {
    CANVAS = 460, MAX_FRAMES = 2048, MAX_PACK_BYTES = 32 * 1024 * 1024,
    POSE_INTERVAL_MS = 120, BREATH_PERIOD_MS = 3600, BREATH_AMPLITUDE_PX = 3,
};

/* Little-endian AWA1 layout shared with tools/pack_ai_avatar.py. */
struct avatar_header {
    char magic[4];
    uint16_t canvas_width, canvas_height, x, y, width, height;
    uint32_t fps, clips, frames, max_compressed;
};
_Static_assert(sizeof(struct avatar_header) == 32, "AWA1 header");
_Static_assert(sizeof(struct ai_avatar_clip) == 56, "AWA1 clip record");

struct avatar_pack {
    struct avatar_header header;
    struct ai_avatar_clip *clips;
    uint32_t *offsets;
    uint8_t *pixels, *next_pixels, *compressed;
    size_t pixels_bytes;
    aster_os_file_t file;
    lv_image_dsc_t descriptor;
    uint32_t frame;
    bool speaking_variant, mouth_open;
#ifdef ASTER_AI_AVATAR_P4_ACCEL
    uint8_t *scaled;
    uint8_t *cached_frames;
    size_t scaled_capacity;
    lv_image_dsc_t scaled_image;
    bool checked_color;
#endif
};

struct avatar {
    struct avatar_pack full, upper;
#ifdef ASTER_AI_AVATAR_P4_ACCEL
    struct avatar_pack close;
    jpeg_decoder_handle_t jpeg;
    ppa_client_handle_t scaler;
#endif
    struct avatar_pack *active;
    lv_obj_t *image;
    lv_timer_t *timer;
    tinfl_decompressor decoder;
    uint32_t clip, after_clip, frame, started;
    int zoom_percent;
    bool loop, frame_error, breathing, cropped_upper;
    struct {
        uint64_t read_us, decode_us, scale_us, started_us;
        struct aster_ui_frame_timing display;
        unsigned frames, mouth_changes;
    } profile;
};

static bool is_hd(const struct avatar_pack *p)
{
#ifdef ASTER_AI_AVATAR_P4_ACCEL
    return !memcmp(p->header.magic, "AWJ1", 4) || !memcmp(p->header.magic, "AWJ2", 4);
#else
    (void)p;
    return false;
#endif
}

#ifdef ASTER_AI_AVATAR_P4_ACCEL
static bool init_hd(struct avatar *a, struct avatar_pack *pack)
{
    const jpeg_decode_engine_cfg_t jpeg_cfg = {.timeout_ms = 60};
    const ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
    if ((!a->jpeg && jpeg_new_decoder_engine(&jpeg_cfg, &a->jpeg) != ESP_OK) ||
        (!a->scaler && ppa_register_client(&ppa_cfg, &a->scaler) != ESP_OK))
        return false;
    const jpeg_decode_memory_alloc_cfg_t cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t capacity;
    pack->pixels = jpeg_alloc_decoder_mem(pack->pixels_bytes, &cfg, &capacity);
    /* Only the visible region, plus one alignment block per edge, is scaled. */
    pack->scaled = jpeg_alloc_decoder_mem(512 * 512 * 2, &cfg, &pack->scaled_capacity);
    return pack->pixels && pack->scaled;
}
#endif

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

static bool load_pack(struct avatar *avatar, struct avatar_pack *a, const char *path)
{
    (void)avatar;
    aster_os_entry_t file;
    if (aster_os_stat(path, &file) || file.type != ASTER_OS_FILE || file.size > MAX_PACK_BYTES ||
        aster_os_file_open(&a->file, path, ASTER_OS_READ))
        return false;
    uint64_t file_bytes = file.size;
    a->frame = UINT32_MAX;
    struct avatar_header *h = &a->header;
    if (!read_exact(&a->file, h, sizeof(*h)) || !h->width || !h->height ||
        !h->clips || h->clips > ASTER_AI_AVATAR_MAX_CLIPS ||
        !h->frames || h->frames > MAX_FRAMES)
        return false;
    bool hd = false;
#ifdef ASTER_AI_AVATAR_P4_ACCEL
    hd = is_hd(a);
    a->speaking_variant = !memcmp(h->magic, "AWJ2", 4);
#endif
    if (hd) {
        /* The overview uses the 2x reference. The closeup is a crop of 4x. */
        bool overview = h->canvas_width == 960 && h->width == 960 && h->height == 1536;
        bool closeup = h->canvas_width == 1920 && h->width == 1120 && h->height == 1120;
        if ((!overview && !closeup) || h->canvas_height != h->canvas_width ||
            h->x >= h->canvas_width || h->y >= h->canvas_height || h->fps < 12 || h->fps > 30)
            return false;
    } else if (memcmp(h->magic, "AWA1", 4) || h->canvas_width != CANVAS || h->canvas_height != CANVAS ||
               h->x + h->width > CANVAS || h->y + h->height > CANVAS || (h->fps != 15 && h->fps != 12)) {
        return false;
    }
    a->pixels_bytes = (size_t)h->width * h->height * (hd ? 2 : 3);
    if (!h->max_compressed || h->max_compressed > a->pixels_bytes + 1024)
        return false;
    size_t clips_bytes = h->clips * sizeof(*a->clips);
    unsigned variants = a->speaking_variant ? 2 : 1;
    size_t index_bytes = (h->frames * variants + 1) * sizeof(*a->offsets);
    size_t data_start = sizeof(*h) + clips_bytes + index_bytes;
    if (file_bytes <= data_start)
        return false;
    a->clips = aster_os_alloc_external(clips_bytes);
    a->offsets = aster_os_alloc_external(index_bytes);
    if (hd) {
#ifdef ASTER_AI_AVATAR_P4_ACCEL
        if (!init_hd(avatar, a))
            return false;
#endif
    } else {
        a->pixels = aster_os_alloc_external(a->pixels_bytes);
        a->next_pixels = aster_os_alloc_external(a->pixels_bytes);
    }
    a->compressed = aster_os_alloc_external(h->max_compressed);
    if (!a->clips || !a->offsets || !a->pixels || (!hd && !a->next_pixels) || !a->compressed ||
        !read_exact(&a->file, a->clips, clips_bytes) || !read_exact(&a->file, a->offsets, index_bytes))
        return false;
    uint32_t frames = 0;
    for (unsigned i = 0; i < h->clips; i++) {
        const struct ai_avatar_clip *clip = &a->clips[i];
        if (clip->first != frames || !clip->frames || clip->frames > h->frames - frames ||
            clip->loop > 1 || !clip->label[0] || !memchr(clip->label, 0, sizeof(clip->label)))
            return false;
        frames += clip->frames;
    }
    unsigned pictures = frames * variants;
    if (frames != h->frames || a->offsets[0] != data_start || a->offsets[pictures] != file_bytes)
        return false;
    for (unsigned i = 0; i < pictures; i++)
        if (a->offsets[i + 1] <= a->offsets[i] ||
            a->offsets[i + 1] - a->offsets[i] > h->max_compressed)
            return false;
#ifdef ASTER_AI_AVATAR_P4_ACCEL
    /* Keep the voice/decode working set available. Large packs stream from SD. */
    size_t cached_size = file_bytes - data_start;
    if (hd && cached_size <= 6 * 1024 * 1024 &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= cached_size + 8 * 1024 * 1024) {
        a->cached_frames = aster_os_alloc_external(cached_size);
        if (a->cached_frames) {
            int64_t started = esp_timer_get_time();
            if (read_exact(&a->file, a->cached_frames, cached_size)) {
                aster_service_logf(3, "ai-avatar: cached JPEG bytes=%u sd_ms=%u", (unsigned)cached_size,
                                   (unsigned)((esp_timer_get_time() - started) / 1000));
                aster_os_free_external(a->compressed);
                a->compressed = NULL;
            } else {
                aster_os_free_external(a->cached_frames);
                a->cached_frames = NULL;
            }
        }
    }
#endif
    a->descriptor = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = hd ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565A8,
                   .w = h->width, .h = h->height, .stride = h->width * 2},
        .data_size = a->pixels_bytes, .data = a->pixels,
    };
    return true;
}

static void release_pack(struct avatar_pack *a)
{
#ifdef ASTER_AI_AVATAR_P4_ACCEL
    aster_os_free_external(a->scaled);
    aster_os_free_external(a->cached_frames);
#endif
    if (a->file.handle)
        aster_os_file_close(&a->file);
    aster_os_free_external(a->compressed);
    aster_os_free_external(a->next_pixels);
    aster_os_free_external(a->pixels);
    aster_os_free_external(a->offsets);
    aster_os_free_external(a->clips);
    *a = (struct avatar_pack){0};
}

static bool unpack_frame(struct avatar *a, struct avatar_pack *pack, uint32_t frame)
{
    if (frame >= pack->header.frames)
        return false;
    bool mouth_open = pack->speaking_variant && ai_dialog_phase() == DIALOG_SPEAKING && ai_reply_audio_voicing();
    if (frame == pack->frame && mouth_open == pack->mouth_open)
        return true;
    unsigned picture = pack->speaking_variant ? frame * 2 + mouth_open : frame;
    size_t input = pack->offsets[picture + 1] - pack->offsets[picture];
    size_t output = pack->pixels_bytes, expected = input;
    const uint8_t *encoded = pack->compressed;
#ifdef ASTER_AI_AVATAR_P4_ACCEL
    if (pack->cached_frames)
        encoded = pack->cached_frames + (pack->offsets[picture] - pack->offsets[0]);
    else
#endif
    {
        int64_t started = esp_timer_get_time();
        if (aster_os_file_seek(&pack->file, pack->offsets[picture], ASTER_OS_SEEK_SET) ||
            !read_exact(&pack->file, pack->compressed, input))
            return false;
        a->profile.read_us += esp_timer_get_time() - started;
    }
    int64_t read_done = esp_timer_get_time();
#ifdef ASTER_AI_AVATAR_P4_ACCEL
    if (is_hd(pack)) {
        const jpeg_decode_cfg_t cfg = {.output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            /* IDF names the little-endian RGB565 DMA order BGR. */
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR, .conv_std = JPEG_YUV_RGB_CONV_STD_BT601};
        jpeg_decode_picture_info_t info;
        uint32_t written = 0;
        if (jpeg_decoder_get_info(encoded, input, &info) != ESP_OK ||
            info.width != pack->header.width || info.height != pack->header.height ||
            jpeg_decoder_process(a->jpeg, &cfg, encoded, input, pack->pixels,
                                 pack->pixels_bytes, &written) != ESP_OK || written != pack->pixels_bytes)
            return false;
        a->profile.decode_us += esp_timer_get_time() - read_done;
        if (mouth_open != pack->mouth_open)
            a->profile.mouth_changes++;
        pack->mouth_open = mouth_open;
        pack->frame = frame;
        return true;
    }
#endif
    tinfl_init(&a->decoder);
    tinfl_status status = tinfl_decompress(&a->decoder, encoded, &input, pack->next_pixels,
        pack->next_pixels, &output, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE || input != expected || output != pack->pixels_bytes)
        return false;
    a->profile.decode_us += esp_timer_get_time() - read_done;
    uint8_t *previous = pack->pixels;
    pack->pixels = pack->next_pixels;
    pack->next_pixels = previous;
    pack->descriptor.data = pack->pixels;
    pack->frame = frame;
    return true;
}

static uint32_t camera_frame(const struct avatar *a, const struct avatar_pack *pack, uint32_t frame)
{
    const struct ai_avatar_clip *full = &a->full.clips[a->clip];
    const struct ai_avatar_clip *shot = &pack->clips[a->clip];
    uint32_t index = frame - full->first;
    if (full->frames > 1)
        index = (index * (shot->frames - 1) + (full->frames - 1) / 2) / (full->frames - 1);
    return shot->first + index;
}

#ifdef ASTER_AI_AVATAR_P4_ACCEL
static bool present_hd(struct avatar *a)
{
    struct avatar_pack *pack = a->active;
    const struct avatar_header *h = &pack->header;
    lv_display_t *display = lv_obj_get_display(a->image);
    int width = lv_display_get_horizontal_resolution(display);
    int height = lv_display_get_vertical_resolution(display);
    int canvas = LV_MIN(width, height);
    /* PPA's scale is in sixteenths. The master shot is 4x the 160% view. */
    int steps = LV_CLAMP(1, (a->zoom_percent * canvas * 16 + h->canvas_width * 80) /
                         (h->canvas_width * 160), 16);
    int blend = LV_CLAMP(0, a->zoom_percent - 245, 215);
    int full_x = (width - h->canvas_width * steps / 16) / 2;
    int pivot_y = canvas * 208 / 1000;
    int full_y = (height - canvas) / 2 + pivot_y - h->canvas_height * steps * 208 / 16000;
    int close_x = width / 2 - h->x * steps / 16;
    int close_y = (height - canvas) / 2 + canvas * 47 / 100 - h->y * steps / 16;
    int x = full_x + (close_x - full_x) * blend / 215;
    int y = full_y + (close_y - full_y) * blend / 215;
    if (h->width < h->canvas_width) {
        int source_x = LV_CLAMP(0, (int)h->x - h->width / 2, h->canvas_width - h->width);
        int source_y = LV_MAX(0, (int)h->y - h->height / 2);
        x += source_x * steps / 16;
        y += source_y * steps / 16;
    }
    int crop_x = LV_MAX(0, -x) / steps * 16;
    int crop_y = LV_MAX(0, -y) / steps * 16;
    x += crop_x * steps / 16;
    y += crop_y * steps / 16;
    int src_w = LV_MIN(h->width - crop_x, (width - x + steps - 1) / steps * 16);
    int src_h = LV_MIN(h->height - crop_y, (height - y + steps - 1) / steps * 16);
    int out_w = src_w * steps / 16, out_h = src_h * steps / 16;
    if (out_w <= 0 || out_h <= 0 || (size_t)out_w * out_h * 2 > pack->scaled_capacity)
        return false;
    const float scale = steps / 16.0f;
    const ppa_srm_oper_config_t cfg = {
        .in = {.buffer = pack->pixels, .pic_w = h->width, .pic_h = h->height,
               .block_w = src_w, .block_h = src_h, .block_offset_x = crop_x, .block_offset_y = crop_y,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = pack->scaled, .buffer_size = pack->scaled_capacity,
                .pic_w = out_w, .pic_h = out_h, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .scale_x = scale, .scale_y = scale, .mode = PPA_TRANS_MODE_BLOCKING,
    };
    int64_t started = esp_timer_get_time();
    if (ppa_do_scale_rotate_mirror(a->scaler, &cfg) != ESP_OK)
        return false;
    a->profile.scale_us += esp_timer_get_time() - started;
    if (!pack->checked_color) {
        pack->checked_color = true;
        aster_service_logf(3, "ai-avatar: RGB565 corner jpeg=%04x ppa=%04x",
                           (unsigned)((const uint16_t *)pack->pixels)[0],
                           (unsigned)((const uint16_t *)pack->scaled)[0]);
    }
    if (lv_image_get_src(a->image) != &pack->scaled_image ||
        pack->scaled_image.header.w != out_w || pack->scaled_image.header.h != out_h) {
        lv_image_cache_drop(&pack->scaled_image);
        pack->scaled_image = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                       .w = out_w, .h = out_h, .stride = out_w * 2},
            .data = pack->scaled, .data_size = out_w * out_h * 2,
        };
        lv_image_set_scale(a->image, LV_SCALE_NONE);
        lv_image_set_src(a->image, &pack->scaled_image);
    }
    lv_obj_set_pos(a->image, x, y);
    lv_obj_invalidate(a->image);
    return true;
}
#endif

static void reset_profile(struct avatar *a)
{
    memset(&a->profile, 0, sizeof(a->profile));
    a->profile.started_us = esp_timer_get_time();
    a->profile.display = aster_idf_ui_frame_timing();
}

static void report_profile(struct avatar *a)
{
    unsigned frames = a->profile.frames;
    struct aster_ui_frame_timing now = aster_idf_ui_frame_timing();
    if (now.renders < a->profile.display.renders || now.flushes < a->profile.display.flushes) {
        reset_profile(a);
        return;
    }
    uint64_t elapsed = esp_timer_get_time() - a->profile.started_us;
    uint64_t flush = now.flush_us - a->profile.display.flush_us;
    uint64_t render = now.render_us - a->profile.display.render_us;
    unsigned draws = now.renders - a->profile.display.renders;
    aster_service_logf(3, "avatar-perf: mode=%s phase=%s zoom=%d frames=%u fps_x10=%u "
                       "sd_us=%u decode_us=%u ppa_us=%u draw_us=%u flush_us=%u mouth_changes=%u",
                       is_hd(a->active) ? "jpeg-ppa" : "deflate-lvgl", ai_dialog_name(ai_dialog_phase()),
                       a->zoom_percent, frames, (unsigned)(frames * 10000000ULL / LV_MAX(1, elapsed)),
                       (unsigned)(a->profile.read_us / frames), (unsigned)(a->profile.decode_us / frames),
                       (unsigned)(a->profile.scale_us / frames),
                       (unsigned)((render > flush ? render - flush : 0) / LV_MAX(1, draws)),
                       (unsigned)(flush / LV_MAX(1, draws)), a->profile.mouth_changes);
    reset_profile(a);
}

static bool decode_frame(struct avatar *a, uint32_t frame)
{
    struct avatar_pack *pack = a->active;
    uint32_t shot = is_hd(pack) ? frame : camera_frame(a, pack, frame);
    uint32_t displayed = pack->frame;
    bool mouth_open = pack->mouth_open;
    if (!unpack_frame(a, pack, shot))
        return false;
    a->frame = frame;
    if (shot != displayed || mouth_open != pack->mouth_open) {
#ifdef ASTER_AI_AVATAR_P4_ACCEL
        if (is_hd(pack)) {
            if (!present_hd(a)) {
                pack->frame = UINT32_MAX;
                return false;
            }
        } else
#endif
        {
            lv_image_cache_drop(&pack->descriptor);
            lv_obj_invalidate(a->image);
        }
        if (++a->profile.frames == 30)
            report_profile(a);
    }
    return true;
}

static void display_frame(struct avatar *a, uint32_t frame)
{
    if (!decode_frame(a, frame)) {
        if (!a->frame_error)
            aster_service_logf(2, "ai-avatar: frame %lu unavailable, keeping previous frame",
                               (unsigned long)frame);
        a->frame_error = true;
    } else if (a->frame_error) {
        a->frame_error = false;
        aster_service_logf(3, "ai-avatar: frame playback recovered");
    }
}

static void start_clip(struct avatar *a, uint32_t clip, uint32_t after_clip)
{
    const struct avatar_pack *clock = is_hd(a->active) ? a->active : &a->full;
    const struct ai_avatar_clip *entry = &clock->clips[clip];
    lv_timer_pause(a->timer);
    a->clip = clip;
    a->after_clip = after_clip;
    a->loop = entry->frames > 1 && after_clip == UINT32_MAX;
    a->frame = entry->first;
    a->started = aster_os_uptime32();
    reset_profile(a);
    a->breathing = false;
    lv_obj_set_style_translate_y(a->image, 0, 0);
    display_frame(a, entry->first);
    lv_obj_clear_flag(a->image, LV_OBJ_FLAG_HIDDEN);
    aster_service_logf(3, "ai-avatar: pose=%s loop=%u ambient=%u", entry->label,
                       (unsigned)a->loop, (unsigned)(entry->frames == 1));
    uint32_t fps = clock->header.fps;
    lv_timer_set_period(a->timer, entry->frames == 1 && !clock->speaking_variant ? POSE_INTERVAL_MS :
                        (1000 + fps - 1) / fps);
    lv_timer_reset(a->timer);
    lv_timer_resume(a->timer);
}

static void breathe(struct avatar *a, uint32_t elapsed)
{
    /* Keep the authored pose intact. Only move it a few native pixels. */
    unsigned half_period = BREATH_PERIOD_MS / 2;
    unsigned phase = elapsed % BREATH_PERIOD_MS;
    unsigned rise = phase < half_period ? phase : BREATH_PERIOD_MS - phase;
    int offset = -(int)((rise * BREATH_AMPLITUDE_PX + half_period / 2) / half_period);
    if (lv_obj_get_style_translate_y(a->image, 0) != offset)
        lv_obj_set_style_translate_y(a->image, offset, 0);
    if (!a->breathing && offset != 0) {
        a->breathing = true;
        aster_service_logf(3, "ai-avatar: ambient motion active");
    }
}

static void play_frame(lv_timer_t *timer)
{
    struct avatar *a = lv_timer_get_user_data(timer);
    const struct avatar_pack *clock = is_hd(a->active) ? a->active : &a->full;
    const struct ai_avatar_clip *clip = &clock->clips[a->clip];
    uint32_t elapsed = aster_os_uptime32() - a->started;
    if (clip->frames == 1) {
        if (a->frame_error || clock->speaking_variant)
            display_frame(a, clip->first);
        breathe(a, elapsed);
        return;
    }
    uint32_t fps = clock->header.fps;
    uint64_t sequence = (uint64_t)elapsed * fps / 1000;
    uint32_t last = clip->frames - 1;
    if (!a->loop && sequence >= clip->frames && a->after_clip != UINT32_MAX &&
        a->frame == clip->first + last) {
        start_clip(a, a->after_clip, UINT32_MAX);
        return;
    }
    uint32_t index = a->loop ? sequence % clip->frames : sequence >= last ? last : sequence;
    /* Schedule against the source clock so SD reads and the UI tick's
     * rounding do not add a delay to every subsequent frame. */
    uint64_t next = ((sequence + 1) * 1000 + fps - 1) / fps;
    lv_timer_set_period(timer, (uint32_t)(next - elapsed));
    display_frame(a, clip->first + index);
}

static void delete_image(lv_event_t *event)
{
    struct avatar *a = lv_event_get_user_data(event);
    lv_timer_delete(a->timer);
    lv_image_cache_drop(&a->active->descriptor);
#ifdef ASTER_AI_AVATAR_P4_ACCEL
    lv_image_cache_drop(&a->upper.scaled_image);
    lv_image_cache_drop(&a->close.scaled_image);
    release_pack(&a->close);
    if (a->scaler)
        ppa_unregister_client(a->scaler);
    if (a->jpeg)
        jpeg_del_decoder_engine(a->jpeg);
#endif
    release_pack(&a->upper);
    release_pack(&a->full);
    aster_os_free_external(a);
}

lv_obj_t *ai_avatar_create(lv_obj_t *parent, bool upper_body)
{
    struct avatar *a = aster_os_alloc_external(sizeof(*a));
    if (!a)
        return NULL;
    memset(a, 0, sizeof(*a));
    a->zoom_percent = 100;
    a->active = &a->full;
    const struct avatar_header *h = &a->full.header;
    if (!load_pack(a, &a->full, ASTER_AI_AVATAR_PATH) || is_hd(&a->full) || !unpack_frame(a, &a->full, 0)) {
        release_pack(&a->full);
        aster_os_free_external(a);
        aster_service_logf(2, "ai-avatar: invalid SD asset");
        return NULL;
    }
    a->timer = lv_timer_create(play_frame, 1000 / h->fps, a);
    if (!a->timer) {
        release_pack(&a->full);
        aster_os_free_external(a);
        return NULL;
    }
    lv_timer_pause(a->timer);
    a->image = lv_image_create(parent);
    if (!a->image) {
        lv_timer_delete(a->timer);
        release_pack(&a->full);
        aster_os_free_external(a);
        return NULL;
    }
    lv_obj_set_user_data(a->image, a);
    lv_obj_add_event_cb(a->image, delete_image, LV_EVENT_DELETE, a);
    lv_obj_clear_flag(a->image, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_pivot(a->image, 0, 0);
    ai_avatar_set_upper_body(a->image, upper_body);
    aster_service_logf(3, "ai-avatar: %ux%u RGB565, %lu clips, %lu FPS",
                       h->canvas_width, h->canvas_height, (unsigned long)h->clips, (unsigned long)h->fps);
    start_clip(a, 0, UINT32_MAX);
    return a->image;
}

struct ai_avatar_catalog ai_avatar_catalog(lv_obj_t *image)
{
    if (!image)
        return (struct ai_avatar_catalog){0};
    const struct avatar *a = lv_obj_get_user_data(image);
    return (struct ai_avatar_catalog){.clips = a->full.clips, .count = a->full.header.clips};
}

int ai_avatar_max_zoom(lv_obj_t *image)
{
    const struct avatar *a = lv_obj_get_user_data(image);
#ifdef ASTER_AI_AVATAR_P4_ACCEL
    if (a->close.file.handle)
        return 560;
#endif
    return is_hd(a->active) ? 320 : 180;
}

static bool matching_shots(const struct avatar_pack *full, const struct avatar_pack *upper)
{
    if (full->header.clips != upper->header.clips)
        return false;
    for (uint32_t i = 0; i < full->header.clips; ++i) {
        const struct ai_avatar_clip *a = &full->clips[i], *b = &upper->clips[i];
        if (strcmp(a->label, b->label) || a->loop != b->loop || (a->frames > 1) != (b->frames > 1))
            return false;
    }
    return true;
}

void ai_avatar_set_upper_body(lv_obj_t *image, bool upper_body)
{
    struct avatar *a = lv_obj_get_user_data(image);
    struct avatar_pack *pack = &a->full;
    if (upper_body) {
        if (!a->upper.file.handle) {
#ifdef ASTER_AI_AVATAR_P4_ACCEL
            if (!load_pack(a, &a->upper, ASTER_AI_AVATAR_UPPER_HD_PATH) || !is_hd(&a->upper) ||
                a->upper.header.canvas_width != 960 || !matching_shots(&a->full, &a->upper) ||
                !unpack_frame(a, &a->upper, 0)) {
                release_pack(&a->upper);
            } else if (load_pack(a, &a->close, ASTER_AI_AVATAR_CLOSE_HD_PATH) && is_hd(&a->close) &&
                       a->close.header.canvas_width == 1920 && matching_shots(&a->upper, &a->close) &&
                       a->upper.header.fps == a->close.header.fps && a->upper.header.frames == a->close.header.frames) {
                for (unsigned i = 0; i < a->close.header.clips; i++) {
                    if (a->close.clips[i].frames != a->upper.clips[i].frames) {
                        release_pack(&a->close);
                        break;
                    }
                }
            } else {
                release_pack(&a->close);
            }
#endif
            if (!a->upper.file.handle && (!load_pack(a, &a->upper, ASTER_AI_AVATAR_UPPER_PATH) ||
                !matching_shots(&a->full, &a->upper)))
                release_pack(&a->upper);
        }
        if (a->upper.file.handle && unpack_frame(a, &a->upper, camera_frame(a, &a->upper, a->frame)))
            pack = &a->upper;
    }
    if (!unpack_frame(a, pack, camera_frame(a, pack, a->frame))) {
        aster_service_logf(2, "ai-avatar: camera frame unavailable, keeping current view");
        return;
    }
    if (pack != a->active)
        lv_image_cache_drop(&a->active->descriptor);
    a->active = pack;
    lv_image_cache_drop(&pack->descriptor);
    lv_image_set_src(image, &pack->descriptor);
    a->cropped_upper = upper_body && pack == &a->full;
    ai_avatar_set_zoom(image, a->zoom_percent);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(image);
    aster_service_logf(3, "ai-avatar: camera=%s",
                       is_hd(pack) ? "hd-jpeg-ppa" : a->cropped_upper ? "upper-scaled" : upper_body ? "upper-native" : "full");
}

void ai_avatar_set_zoom(lv_obj_t *image, int percent)
{
    struct avatar *a = lv_obj_get_user_data(image);
    a->zoom_percent = percent;
#ifdef ASTER_AI_AVATAR_P4_ACCEL
    if (is_hd(a->active)) {
        struct avatar_pack *previous = a->active;
        struct avatar_pack *next = percent >= 320 && a->close.file.handle ? &a->close : &a->upper;
        if (next != previous && unpack_frame(a, next, a->frame))
            a->active = next;
        if (!present_hd(a))
            aster_service_logf(2, "ai-avatar: hardware scale failed");
        if (previous != a->active) {
            reset_profile(a);
            aster_service_logf(3, "ai-avatar: camera=%s", a->active == &a->close ? "hd-close" : "hd-overview");
        }
        return;
    }
#endif
    lv_display_t *display = lv_obj_get_display(image);
    int width = lv_display_get_horizontal_resolution(display);
    int height = lv_display_get_vertical_resolution(display);
    int canvas = LV_MIN(width, height);
    const struct avatar_header *h = &a->active->header;
    int base_scale = a->cropped_upper ? 416 : LV_SCALE_NONE;
    int scale = (base_scale * canvas * percent + CANVAS * 50) / (CANVAS * 100);
    int frame_y = (a->cropped_upper ? 24 : h->y) * canvas / CANVAS;
    int anchor_y = canvas / 3;
    int x = width / 2 + ((int)h->x - CANVAS / 2) * scale / LV_SCALE_NONE;
    int y = (height - canvas) / 2 + anchor_y + (frame_y - anchor_y) * percent / 100;
    lv_image_set_scale(image, scale);
    lv_obj_set_pos(image, x, y);
}

static uint32_t clip_index(const struct avatar *a, const char *label)
{
    for (unsigned i = 0; i < a->full.header.clips; i++)
        if (!strcmp(a->full.clips[i].label, label))
            return i;
    return UINT32_MAX;
}

void ai_avatar_perform(lv_obj_t *image, const struct ai_avatar_cue *cue)
{
    struct avatar *a = lv_obj_get_user_data(image);
    uint32_t motion = clip_index(a, cue->motion);
    uint32_t expression = clip_index(a, cue->expression);
    if (motion != UINT32_MAX)
        start_clip(a, motion, expression);
    else if (expression != UINT32_MAX)
        start_clip(a, expression, UINT32_MAX);
}
