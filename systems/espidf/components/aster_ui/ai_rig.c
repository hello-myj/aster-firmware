/* SPDX-License-Identifier: MIT */
#include "ai_avatar.h"
#include "ai_dialog.h"
#include "ai_reply_audio.h"
#include "ai_rig_draw.h"
#include "ai_rig_model.h"
#include "aster_os.h"
#include "aster_power.h"
#include "aster_services.h"
#include "ui_font.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

enum { MAX_ATLAS_BYTES = 8 * 1024 * 1024, MAX_DAMAGE_VERTICES = 128 * 1024 };
static atomic_bool live_owner;

enum rig_extra {
    RIG_BLUSH, RIG_HEART, RIG_SWIRL, RIG_TEARS, RIG_SQUINT, RIG_ARM_UP, RIG_FINGER_UP, RIG_CHEST_ARM,
    RIG_EYE_SMILE, RIG_LEAN, RIG_BREATH, RIG_EXTRA_COUNT,
};

static const char *const extra_ids[RIG_EXTRA_COUNT] = {
    "key7", "key4", "key3", "key2", "key6", "Movement1", "Movement3", "Movement2", "EyeSmile", "LeanInput", "ParamBreath",
};
#define RIG_ID(name, id) [AI_JOINT_##name] = #id,
static const char *const joint_ids[] = {ASTER_RIG_JOINTS(RIG_ID)};
#undef RIG_ID

static const struct ai_avatar_clip catalog[] = {
    {.frames = 2, .label = "待机"}, {.frames = 2, .label = "点头"}, {.frames = 2, .label = "左右摇头"},
    {.frames = 2, .label = "竖食指"}, {.frames = 2, .label = "挥手"}, {.frames = 2, .label = "身体左右摇"},
    {.frames = 1, .label = "爱心眼"}, {.frames = 1, .label = "圈圈眼"}, {.frames = 1, .label = "脸红"},
    {.frames = 1, .label = "流泪"}, {.frames = 1, .label = "屑屑眼"}, {.frames = 1, .label = "眯眼笑"},
    {.frames = 1, .label = "惊讶"}, {.frames = 1, .label = "前倾"},
};

struct live_rig {
    atomic_uint refs;
    atomic_bool stop, pending, gestures, idle, raster_done;
    atomic_int zoom, error;
    uint64_t joints;
    bool owns_worker_slot;
    portMUX_TYPE guard;
    struct ai_avatar_cue next;
    unsigned revision;
    lv_obj_t *image, *status;
    lv_timer_t *timer;
    lv_image_dsc_t descriptor;
    uint16_t *frames[2];
    unsigned ready_index;
    struct rig_tiles ready_tiles;
    struct rig_damage damage;
    struct rig_draw draw[2];
    struct rig_camera camera;
    void *atlas, *moc, *state;
    int controls[AI_JOINT_COUNT], extra[RIG_EXTRA_COUNT];
    TaskHandle_t owner;
    csmModel *render_model;
    uint16_t *render_pixels;
    int render_zoom;
};

static void *external(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void release(struct live_rig *rig)
{
    if (atomic_fetch_sub(&rig->refs, 1) != 1)
        return;
    for (unsigned band = 0; band < 2; band++)
        for (unsigned i = 0; i < RIG_MASK_PAGES; i++)
            heap_caps_free(rig->draw[band].masks[i].pixels);
    heap_caps_free(rig->frames[0]);
    heap_caps_free(rig->frames[1]);
    heap_caps_free(rig->damage.meshes);
    heap_caps_free(rig->damage.vertices);
    heap_caps_free(rig->atlas);
    heap_caps_free(rig->moc);
    heap_caps_free(rig->state);
    bool owned = rig->owns_worker_slot;
    heap_caps_free(rig);
    if (owned)
        atomic_store(&live_owner, false);
}

static int load_atlas(struct live_rig *rig)
{
    static const char path[] = "/SD:/aster/avatar-live.atx";
    aster_os_entry_t entry;
    if (aster_os_stat(path, &entry) || entry.type != ASTER_OS_FILE)
        return -ENOENT;
    if (entry.size < 8 || entry.size > MAX_ATLAS_BYTES)
        return -EINVAL;
    uint8_t *buffer = external(entry.size);
    if (!buffer)
        return -ENOMEM;
    rig->atlas = buffer;
    aster_os_file_t file = {0};
    int rc = aster_os_file_open(&file, path, ASTER_OS_READ);
    if (rc)
        return rc;
    size_t done = 0;
    while (done < entry.size && !atomic_load(&rig->stop)) {
        size_t left = entry.size - done;
        int n = aster_os_file_read(&file, buffer + done, left < 65536 ? left : 65536);
        if (n <= 0)
            break;
        done += n;
        vTaskDelay(1);
    }
    aster_os_file_close(&file);
    if (done != entry.size || memcmp(buffer, "ATX1", 4) || buffer[4] != RIG_TEXTURES)
        return -EIO;
    size_t pos = 8;
    for (unsigned i = 0; i < RIG_TEXTURES; i++) {
        uint32_t dims[2];
        if (done - pos < sizeof(dims))
            return -EINVAL;
        memcpy(dims, buffer + pos, sizeof(dims));
        pos += sizeof(dims);
        if (!dims[0] || !dims[1] || dims[0] > 2048 || dims[1] > 2048 || (dims[0] % 4))
            return -EINVAL;
        size_t pixels = dims[0] * dims[1];
        size_t bytes = pixels * 2 + pixels / 2;
        if (bytes > done - pos)
            return -EINVAL;
        struct rig_texture texture = {dims[0], dims[1], (const uint16_t *)(buffer + pos), buffer + pos + pixels * 2};
        rig->draw[0].textures[i] = rig->draw[1].textures[i] = texture;
        pos += bytes;
    }
    return pos == done ? 0 : -EINVAL;
}

static csmModel *load_model(struct live_rig *rig)
{
    unsigned bytes;
    int rc = ai_rig_read_moc(&rig->moc, &bytes);
    if (rc) {
        atomic_store(&rig->error, rc);
        return NULL;
    }
    csmMoc *moc = csmReviveMocInPlace(rig->moc, bytes);
    unsigned size = moc ? csmGetSizeofModel(moc) : 0;
    if (!size) {
        atomic_store(&rig->error, -EINVAL);
        return NULL;
    }
    rig->state = heap_caps_aligned_alloc(csmAlignofModel, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rig->state) {
        atomic_store(&rig->error, -ENOMEM);
        return NULL;
    }
    csmModel *model = csmInitializeModelInPlace(moc, rig->state, size);
    if (!model || csmGetDrawableCount(model) > RIG_MAX_MESHES) {
        atomic_store(&rig->error, -EINVAL);
        return NULL;
    }
    rc = atomic_load(&rig->stop) ? -ECANCELED : load_atlas(rig);
    if (rc) {
        atomic_store(&rig->error, rc);
        return NULL;
    }
    size_t area = RIG_WIDTH * RIG_HEIGHT;
    for (unsigned band = 0; band < 2; band++) {
        for (unsigned i = 0; i < RIG_MASK_PAGES; i++) {
            rig->draw[band].masks[i].pixels = external(area / 2);
            if (!rig->draw[band].masks[i].pixels) {
                atomic_store(&rig->error, -ENOMEM);
                return NULL;
            }
        }
    }
    for (unsigned i = 0; i < 2; i++) {
        rig->frames[i] = external(area * 2);
        if (!rig->frames[i]) {
            atomic_store(&rig->error, -ENOMEM);
            return NULL;
        }
    }
    unsigned mesh_count = csmGetDrawableCount(model), vertex_count = 0;
    const int *counts = csmGetDrawableVertexCounts(model);
    for (unsigned id = 0; id < mesh_count; id++)
        vertex_count += counts[id];
    if (vertex_count && vertex_count <= MAX_DAMAGE_VERTICES) {
        rig->damage.meshes = external(mesh_count * sizeof(*rig->damage.meshes));
        rig->damage.vertices = external(vertex_count * sizeof(*rig->damage.vertices));
        if (rig->damage.meshes && rig->damage.vertices) {
            memset(rig->damage.meshes, 0, mesh_count * sizeof(*rig->damage.meshes));
            unsigned offset = 0;
            for (unsigned id = 0; id < mesh_count; id++) {
                rig->damage.meshes[id].vertex_offset = offset;
                offset += counts[id];
            }
            for (unsigned band = 0; band < 2; band++)
                rig->draw[band].meshes = rig->damage.meshes;
        } else {
            heap_caps_free(rig->damage.meshes);
            heap_caps_free(rig->damage.vertices);
            rig->damage = (struct rig_damage){0};
        }
    }
    uint64_t available = 0;
    for (unsigned i = 0; i < AI_JOINT_COUNT; i++) {
        rig->controls[i] = ai_rig_param_index(model, joint_ids[i]);
        if (rig->controls[i] >= 0)
            available |= UINT64_C(1) << i;
    }
    for (unsigned i = 0; i < RIG_EXTRA_COUNT; i++)
        rig->extra[i] = ai_rig_param_index(model, extra_ids[i]);
    csmUpdateModel(model);
    ai_rig_fit(model, &rig->camera);
    portENTER_CRITICAL(&rig->guard);
    rig->joints = available;
    portEXIT_CRITICAL(&rig->guard);
    aster_service_logf(3, "ai-live: model ready params=%d meshes=%d psram=%u damage=%u", csmGetParameterCount(model),
                       csmGetDrawableCount(model), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                       rig->damage.vertices != NULL);
    return model;
}

struct rig_parameters { float *values; const float *base, *lo, *hi; };

static void set(struct rig_parameters *p, int index, float x)
{
    if (index >= 0)
        p->values[index] = p->base[index] + x * (x >= 0 ? p->hi[index] - p->base[index] : p->base[index] - p->lo[index]);
}

static void expression(const struct live_rig *rig, struct rig_parameters *p, const char *name)
{
    if (!strcmp(name, "爱心眼")) set(p, rig->extra[RIG_HEART], 1);
    else if (!strcmp(name, "圈圈眼")) set(p, rig->extra[RIG_SWIRL], 1);
    else if (!strcmp(name, "脸红")) set(p, rig->extra[RIG_BLUSH], 1);
    else if (!strcmp(name, "流泪")) set(p, rig->extra[RIG_TEARS], 1);
    else if (!strcmp(name, "屑屑眼")) set(p, rig->extra[RIG_SQUINT], 1);
    else if (!strcmp(name, "前倾")) set(p, rig->extra[RIG_LEAN], -1);
    else if (!strcmp(name, "眯眼笑")) {
        set(p, rig->extra[RIG_EYE_SMILE], .85f);
        set(p, rig->controls[AI_JOINT_left_eye], -.70f);
        set(p, rig->controls[AI_JOINT_right_eye], -.70f);
        set(p, rig->controls[AI_JOINT_mouth_form], .8f);
    } else if (!strcmp(name, "惊讶")) {
        set(p, rig->controls[AI_JOINT_left_eye], .36f);
        set(p, rig->controls[AI_JOINT_right_eye], .36f);
        set(p, rig->controls[AI_JOINT_jaw], .5f);
    }
}

static void motion(const struct live_rig *rig, struct rig_parameters *p, const char *name, float seconds)
{
    float wave = sinf(seconds * 5.0f);
    if (!strcmp(name, "点头")) set(p, rig->controls[AI_JOINT_head_pitch], wave * .55f);
    else if (!strcmp(name, "左右摇头")) set(p, rig->controls[AI_JOINT_head_yaw], wave * .58f);
    else if (!strcmp(name, "竖食指") || !strcmp(name, "挥手")) {
        bool point = !strcmp(name, "竖食指");
        set(p, rig->extra[point ? RIG_FINGER_UP : RIG_ARM_UP], 1);
        set(p, rig->controls[AI_JOINT_raised_left_shoulder], .25f);
        set(p, rig->controls[AI_JOINT_raised_left_elbow], point ? .15f : wave * .3f);
        set(p, rig->controls[AI_JOINT_raised_left_wrist], point ? .10f : wave * .50f);
    } else if (!strcmp(name, "身体左右摇")) {
        set(p, rig->controls[AI_JOINT_body_roll], wave * .45f);
        set(p, rig->controls[AI_JOINT_head_roll], wave * -.2f);
        set(p, rig->controls[AI_JOINT_tail_2], wave * .3f);
    }
}

static void update_pose(struct live_rig *rig, csmModel *model, const struct ai_avatar_cue *cue,
                        float seconds, float elapsed, bool speaking, float mouth)
{
    struct rig_parameters p = {csmGetParameterValues(model), csmGetParameterDefaultValues(model),
                               csmGetParameterMinimumValues(model), csmGetParameterMaximumValues(model)};
    memcpy(p.values, p.base, csmGetParameterCount(model) * sizeof(float));
    set(&p, rig->extra[RIG_BREATH], .45f + .4f * sinf(seconds * 1.9f));
    float blink = 1 - fabsf(fmodf(seconds, 4.8f) - 2.9f) / .18f;
    if (blink > 0) {
        set(&p, rig->controls[AI_JOINT_left_eye], -blink);
        set(&p, rig->controls[AI_JOINT_right_eye], -blink);
    }
    if (!cue->motion[0] && !cue->joint_mask) {
        if (speaking && atomic_load(&rig->gestures)) {
            set(&p, rig->controls[AI_JOINT_head_pitch], .13f * sinf(seconds * 3.0f));
            set(&p, rig->controls[AI_JOINT_head_roll], .10f * sinf(seconds * 1.6f));
        } else if (!speaking && atomic_load(&rig->idle) && fmodf(seconds, 10.0f) > 8.0f) {
            set(&p, rig->controls[AI_JOINT_head_yaw], .28f * sinf(seconds * 3.7f));
            set(&p, rig->controls[AI_JOINT_tail_2], .23f * sinf(seconds * 3.7f));
        }
    }
    motion(rig, &p, cue->motion, elapsed);
    expression(rig, &p, cue->expression);
    const uint64_t raised = ((UINT64_C(1) << 5) - 1) << AI_JOINT_raised_left_shoulder;
    const uint64_t chest = ((UINT64_C(1) << 4) - 1) << AI_JOINT_chest_right_shoulder;
    if (cue->joint_mask & raised) set(&p, rig->extra[RIG_ARM_UP], 1);
    if (cue->joint_mask & chest) set(&p, rig->extra[RIG_CHEST_ARM], 1);
    for (unsigned i = 0; i < AI_JOINT_COUNT; i++) {
        if (cue->joint_mask & (UINT64_C(1) << i))
            set(&p, rig->controls[i], cue->joints[i] / 100.0f * fminf(elapsed * 5, 1));
    }
    if (speaking && !(cue->joint_mask & (UINT64_C(1) << AI_JOINT_jaw))) {
        set(&p, rig->controls[AI_JOINT_jaw], mouth);
        set(&p, rig->controls[AI_JOINT_jaw_hinge], mouth * .4f);
    }
    csmUpdateModel(model);
}

static void draw_lower_band(void *argument)
{
    struct live_rig *rig = argument;
    TaskHandle_t owner = rig->owner;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_load(&rig->raster_done))
            break;
        ai_rig_draw(&rig->draw[1], rig->render_model, &rig->camera,
                    rig->render_pixels + RIG_WIDTH * (RIG_HEIGHT / 2), rig->render_zoom);
        xTaskNotifyGive(owner);
    }
    xTaskNotifyGive(owner);
    vTaskDelete(NULL);
}

static void sync_back_buffer(uint16_t *back, const uint16_t *front,
                             const struct rig_tiles *last, const struct rig_tiles *next)
{
    /* The back buffer is one published frame behind. Copy last frame's
     * changed tiles that this frame will keep, then paint this frame's tiles. */
    for (unsigned row = 0; row < RIG_TILE_ROWS; row++) {
        unsigned bits = last->rows[row] & ~next->rows[row];
        while (bits) {
            unsigned first;
            unsigned end = ai_rig_take_span(&bits, &first);
            size_t offset = row * RIG_TILE_SIZE * RIG_WIDTH + first * RIG_TILE_SIZE;
            size_t bytes = (end - first) * RIG_TILE_SIZE * sizeof(*back);
            for (unsigned y = 0; y < RIG_TILE_SIZE; y++, offset += RIG_WIDTH)
                memcpy(back + offset, front + offset, bytes);
        }
    }
}

static void run(void *argument)
{
    struct live_rig *rig = argument;
    while (!atomic_load(&rig->stop)) {
        if (!atomic_exchange(&live_owner, true)) {
            rig->owns_worker_slot = true;
            break;
        }
        vTaskDelay(1);
    }
    if (!rig->owns_worker_slot) {
        release(rig);
        vTaskDelete(NULL);
        return;
    }
    int rc = aster_power_activity_begin();
    csmModel *model = NULL;
    if (!rc) {
        model = load_model(rig);
        aster_power_activity_end();
    } else {
        atomic_store(&rig->error, rc);
    }
    if (!model) {
        aster_service_logf(2, "ai-live: load failed %d", atomic_load(&rig->error));
        release(rig);
        vTaskDelete(NULL);
        return;
    }
    int64_t start = esp_timer_get_time(), cue_at = start, window = start;
    uint64_t solve = 0, damage_time = 0, draw = 0;
    unsigned back = 0, frames = 0, updates = 0, tiles = 0, last_revision = 0, voiced_frames = 0;
    struct rig_tiles last_tiles = {0};
    struct ai_avatar_cue cue = {0};
    bool was_speaking = false;
    float mouth = 0;
    rig->owner = xTaskGetCurrentTaskHandle();
    rig->render_model = model;
    TaskHandle_t raster = NULL;
    if (xTaskCreatePinnedToCore(draw_lower_band, "avatar-raster", 4096, rig, 2, &raster, 1) != pdPASS)
        raster = NULL;
    aster_service_logf(3, "ai-live: raster cores=%u", raster ? 2U : 1U);
    while (!atomic_load(&rig->stop)) {
        if (atomic_load(&rig->pending)) {
            vTaskDelay(1);
            continue;
        }
        int64_t before = esp_timer_get_time();
        portENTER_CRITICAL(&rig->guard);
        if (rig->revision != last_revision) {
            last_revision = rig->revision;
            cue = rig->next;
            cue_at = before;
        }
        portEXIT_CRITICAL(&rig->guard);
        bool speaking = ai_dialog_phase() == DIALOG_SPEAKING;
        if (!speaking && !cue.keep_pose && (was_speaking || before - cue_at > 2500000))
            cue = (struct ai_avatar_cue){0};
        was_speaking = speaking;
        float level = speaking ? ai_reply_audio_level() / 100.0f : 0;
        mouth = level * .82f + mouth * .18f;
        update_pose(rig, model, &cue, (before - start) / 1000000.0f, (before - cue_at) / 1000000.0f,
                    speaking, mouth);
        int64_t posed = esp_timer_get_time();
        int zoom = atomic_load(&rig->zoom);
        unsigned count = ai_rig_collect_damage(&rig->damage, model, &rig->camera, zoom, &rig->ready_tiles);
        int64_t compared = esp_timer_get_time();
        solve += posed - before;
        damage_time += compared - posed;
        if (count) {
            sync_back_buffer(rig->frames[back], rig->frames[back ^ 1], &last_tiles, &rig->ready_tiles);
            rig->render_pixels = rig->frames[back];
            rig->render_zoom = zoom;
            if (raster)
                xTaskNotifyGive(raster);
            ai_rig_draw(&rig->draw[0], model, &rig->camera, rig->render_pixels, zoom);
            if (raster)
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            else
                ai_rig_draw(&rig->draw[1], model, &rig->camera, rig->render_pixels + RIG_WIDTH * (RIG_HEIGHT / 2), zoom);
            draw += esp_timer_get_time() - compared;
            last_tiles = rig->ready_tiles;
            rig->ready_index = back;
            atomic_store(&rig->pending, true);
            back ^= 1;
            frames++;
            tiles += count;
            voiced_frames += level > .05f;
        }
        if (++updates == 10) {
            int64_t now = esp_timer_get_time();
            unsigned divisor = frames ? frames : 1;
            aster_service_logf(3, "ai-live: frames=%u fps_x10=%u eval_x10=%u zoom=%d tiles=%u pose_us=%u diff_us=%u draw_us=%u voiced=%u phase=%s", frames,
                               (unsigned)(frames * 10000000ULL / (now - window)),
                               (unsigned)(updates * 10000000ULL / (now - window)), zoom, tiles / divisor,
                               (unsigned)(solve / updates), (unsigned)(damage_time / updates), (unsigned)(draw / divisor),
                               voiced_frames, ai_dialog_name(ai_dialog_phase()));
            frames = updates = tiles = voiced_frames = 0;
            solve = damage_time = draw = 0;
            window = now;
        }
        vTaskDelay(1);
    }
    if (raster) {
        atomic_store(&rig->raster_done, true);
        xTaskNotifyGive(raster);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    release(rig);
    vTaskDelete(NULL);
}

static void invalidate_tiles(lv_obj_t *image, struct rig_tiles tiles)
{
    lv_area_t bounds;
    lv_obj_get_coords(image, &bounds);
    for (unsigned row = 0; row < RIG_TILE_ROWS; row++) {
        while (tiles.rows[row]) {
            unsigned first, bits = tiles.rows[row];
            unsigned end = ai_rig_take_span(&bits, &first);
            unsigned run = tiles.rows[row] ^ bits;
            tiles.rows[row] = bits;
            unsigned bottom = row + 1;
            while (bottom < RIG_TILE_ROWS && (tiles.rows[bottom] & run) == run) {
                tiles.rows[bottom] &= ~run;
                bottom++;
            }
            lv_area_t area = {bounds.x1 + first * RIG_TILE_SIZE, bounds.y1 + row * RIG_TILE_SIZE,
                bounds.x1 + end * RIG_TILE_SIZE - 1, bounds.y1 + bottom * RIG_TILE_SIZE - 1};
            lv_obj_invalidate_area(image, &area);
        }
    }
}

static void show_frame(lv_timer_t *timer)
{
    struct live_rig *rig = lv_timer_get_user_data(timer);
    int error = atomic_load(&rig->error);
    if (error) {
        lv_label_set_text(rig->status, error == -ENOMEM ? "角色内存不足" : "无法加载实时角色素材");
        lv_timer_pause(timer);
        return;
    }
    if (!atomic_load(&rig->pending))
        return;
    bool first = rig->descriptor.data == NULL;
    lv_image_cache_drop(&rig->descriptor);
    rig->descriptor.data = (const uint8_t *)rig->frames[rig->ready_index];
    if (first) {
        lv_image_set_src(rig->image, &rig->descriptor);
        lv_obj_add_flag(rig->status, LV_OBJ_FLAG_HIDDEN);
        aster_service_logf(3, "ai-live: first RGB565 frame 480x480");
    } else {
        invalidate_tiles(rig->image, rig->ready_tiles);
    }
    atomic_store(&rig->pending, false);
}

static void close_image(lv_event_t *event)
{
    struct live_rig *rig = lv_event_get_user_data(event);
    lv_timer_delete(rig->timer);
    lv_image_cache_drop(&rig->descriptor);
    lv_image_set_src(rig->image, NULL);
    atomic_store(&rig->stop, true);
    release(rig);
}

lv_obj_t *ai_avatar_create(lv_obj_t *parent, bool upper_body)
{
    (void)upper_body;
    struct live_rig *rig = external(sizeof(*rig));
    if (!rig)
        return NULL;
    memset(rig, 0, sizeof(*rig));
    portMUX_INITIALIZE(&rig->guard);
    atomic_init(&rig->refs, 1);
    atomic_init(&rig->zoom, 160);
    atomic_init(&rig->gestures, true);
    for (unsigned band = 0; band < 2; band++) {
        rig->draw[band].width = RIG_WIDTH;
        rig->draw[band].height = RIG_HEIGHT / 2;
        rig->draw[band].canvas_height = RIG_HEIGHT;
        rig->draw[band].y_offset = band * (RIG_HEIGHT / 2);
        rig->draw[band].tiles = &rig->ready_tiles;
    }
    rig->descriptor = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
        .w = RIG_WIDTH, .h = RIG_HEIGHT, .stride = RIG_WIDTH * 2}, .data_size = RIG_WIDTH * RIG_HEIGHT * 2};
    rig->image = lv_image_create(parent);
    lv_obj_remove_flag(rig->image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(rig->image, RIG_WIDTH, RIG_HEIGHT);
    rig->status = lv_label_create(rig->image);
    lv_obj_set_style_text_font(rig->status, aster_ui_font(16), 0);
    lv_obj_set_style_text_color(rig->status, lv_color_white(), 0);
    lv_label_set_text(rig->status, "正在加载实时角色…");
    lv_obj_center(rig->status);
    rig->timer = lv_timer_create(show_frame, 30, rig);
    if (!rig->timer) {
        lv_obj_delete(rig->image);
        release(rig);
        return NULL;
    }
    lv_obj_set_user_data(rig->image, rig);
    lv_obj_add_event_cb(rig->image, close_image, LV_EVENT_DELETE, rig);
    atomic_fetch_add(&rig->refs, 1);
    if (xTaskCreatePinnedToCore(run, "live-avatar", 12288, rig, 2, NULL, 0) != pdPASS) {
        atomic_store(&rig->error, -ENOMEM);
        release(rig);
    }
    return rig->image;
}

struct ai_avatar_catalog ai_avatar_catalog(lv_obj_t *image)
{
    if (!image)
        return (struct ai_avatar_catalog){0};
    struct live_rig *rig = lv_obj_get_user_data(image);
    portENTER_CRITICAL(&rig->guard);
    uint64_t supported = rig->joints;
    portEXIT_CRITICAL(&rig->guard);
    return (struct ai_avatar_catalog){.clips = catalog, .count = supported ? sizeof(catalog) / sizeof(catalog[0]) : 0,
        .joint_mask = supported, .explicit_only = !atomic_load(&rig->gestures)};
}

void ai_avatar_perform(lv_obj_t *image, const struct ai_avatar_cue *cue)
{
    struct live_rig *rig = lv_obj_get_user_data(image);
    portENTER_CRITICAL(&rig->guard);
    rig->next = *cue;
    rig->revision++;
    portEXIT_CRITICAL(&rig->guard);
    aster_service_logf(3, "ai-live: cue motion=%s expression=%s joints=%llx", cue->motion,
                       cue->expression, (unsigned long long)cue->joint_mask);
}

void ai_avatar_set_zoom(lv_obj_t *image, int percent)
{
    struct live_rig *rig = lv_obj_get_user_data(image);
    atomic_store(&rig->zoom, percent);
}

int ai_avatar_max_zoom(lv_obj_t *image)
{
    (void)image;
    return 560;
}

void ai_avatar_set_autonomy(lv_obj_t *image, bool gestures, bool idle)
{
    struct live_rig *rig = lv_obj_get_user_data(image);
    atomic_store(&rig->gestures, gestures);
    atomic_store(&rig->idle, idle);
}
