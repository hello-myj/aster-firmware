/* SPDX-License-Identifier: MIT */
#include "ai_rig_probe.h"
#include "ai_rig_model.h"
#include "PurismCore.h"
#include "aster_os.h"
#include "aster_power.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

static const char tag[] = "ai_rig";
static atomic_bool running;

int ai_rig_read_moc(void **out, unsigned *size)
{
    static const char model_path[] = "/SD:/aster/avatar-live.moc3";
    aster_os_entry_t entry;
    if (aster_os_stat(model_path, &entry) || entry.type != ASTER_OS_FILE)
        return -ENOENT;
    if (!entry.size || entry.size > 20U * 1024 * 1024)
        return -EFBIG;
    size_t bytes = entry.size;
    uint8_t *buffer = heap_caps_aligned_alloc(csmAlignofMoc, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer)
        return -ENOMEM;
    *out = buffer;
    aster_os_file_t file = {0};
    int error = aster_os_file_open(&file, model_path, ASTER_OS_READ);
    if (error)
        return error;
    size_t position = 0;
    while (position < bytes) {
        size_t count = bytes - position;
        if (count > 64 * 1024)
            count = 64 * 1024;
        int n = aster_os_file_read(&file, buffer + position, count);
        if (n <= 0 || (size_t)n > count) {
            error = -EIO;
            break;
        }
        position += n;
        vTaskDelay(1);
    }
    aster_os_file_close(&file);
    *size = (unsigned)bytes;
    return error;
}

int ai_rig_param_index(const csmModel *model, const char *id)
{
    const char **ids = csmGetParameterIds(model);
    int count = csmGetParameterCount(model);
    for (int i = 0; i < count; i++) {
        if (!strcmp(ids[i], id))
            return i;
    }
    return -1;
}

static uint32_t vertex_signature(const csmModel *model)
{
    const csmVector2 **vertices = csmGetDrawableVertexPositions(model);
    const int *counts = csmGetDrawableVertexCounts(model);
    uint32_t hash = 2166136261U;
    int drawables = csmGetDrawableCount(model);
    for (int i = 0; i < drawables; i++) {
        const uint8_t *bytes = (const uint8_t *)vertices[i];
        size_t count = (size_t)counts[i] * sizeof(**vertices);
        for (size_t j = 0; j < count; j++)
            hash = (hash ^ bytes[j]) * 16777619U;
    }
    return hash;
}

static int measure(csmModel *model)
{
    const char *const controls[] = {"ParamAngleX", "LForearm", "ParamMouthOpenY"};
    float *values = csmGetParameterValues(model);
    const float *defaults = csmGetParameterDefaultValues(model);
    const float *maximum = csmGetParameterMaximumValues(model);
    int indices[sizeof(controls) / sizeof(controls[0])];
    csmUpdateModel(model);
    uint32_t base = vertex_signature(model);
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        int index = ai_rig_param_index(model, controls[i]);
        if (index < 0) {
            ESP_LOGE(tag, "missing parameter %s", controls[i]);
            return -ENOENT;
        }
        indices[i] = index;
        values[index] = maximum[index];
        csmUpdateModel(model);
        uint32_t moved = vertex_signature(model);
        values[index] = defaults[index];
        csmUpdateModel(model);
        ESP_LOGI(tag, "parameter %s mesh=%08lx changed=%d reset=%d", controls[i],
                 (unsigned long)moved, moved != base, vertex_signature(model) == base);
        if (moved == base)
            return -ERANGE;
    }
    enum { UPDATES = 30 };
    uint64_t total = 0, longest = 0;
    for (unsigned i = 0; i < UPDATES; i++) {
        float weight = (i % 10) / 9.0f;
        for (size_t j = 0; j < sizeof(indices) / sizeof(indices[0]); j++) {
            int index = indices[j];
            values[index] = defaults[index] + weight * (maximum[index] - defaults[index]);
        }
        int64_t start = esp_timer_get_time();
        csmUpdateModel(model);
        uint64_t elapsed = esp_timer_get_time() - start;
        total += elapsed;
        if (elapsed > longest)
            longest = elapsed;
        vTaskDelay(1);
    }
    ESP_LOGI(tag, "evaluation updates=%d avg_us=%llu max_us=%llu", UPDATES,
             (unsigned long long)(total / UPDATES), (unsigned long long)longest);
    return 0;
}

static void run(void *arg)
{
    (void)arg;
    void *moc_memory = NULL, *model_memory = NULL;
    unsigned moc_size = 0;
    int64_t start = esp_timer_get_time();
    int error = ai_rig_read_moc(&moc_memory, &moc_size);
    if (error)
        goto done;
    ESP_LOGI(tag, "moc bytes=%u sd_ms=%u", moc_size, (unsigned)((esp_timer_get_time() - start) / 1000));
    csmMoc *moc = csmReviveMocInPlace(moc_memory, moc_size);
    if (!moc) {
        error = -EINVAL;
        goto done;
    }
    unsigned size = csmGetSizeofModel(moc);
    ESP_LOGI(tag, "model bytes=%u psram_free=%u psram_largest=%u", size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    model_memory = size ? heap_caps_aligned_alloc(csmAlignofModel, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!model_memory) {
        error = -ENOMEM;
        goto done;
    }
    csmModel *model = csmInitializeModelInPlace(moc, model_memory, size);
    if (!model) {
        error = -EINVAL;
        goto done;
    }
    int drawables = csmGetDrawableCount(model);
    const int *counts = csmGetDrawableIndexCounts(model);
    const int *masks = csmGetDrawableMaskCounts(model);
    unsigned triangles = 0, masked = 0;
    for (int i = 0; i < drawables; i++) {
        triangles += counts[i] / 3;
        masked += masks[i] > 0;
    }
    ESP_LOGI(tag, "params=%d drawables=%d triangles=%u masked=%u psram_free=%u",
             csmGetParameterCount(model), drawables, triangles, masked,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    error = measure(model);
done:
    heap_caps_free(model_memory);
    heap_caps_free(moc_memory);
    ESP_LOGI(tag, "result=%d", error);
    aster_power_activity_end();
    atomic_store(&running, false);
    vTaskDelete(NULL);
}

int ai_rig_bench_start(void)
{
    if (atomic_exchange(&running, true))
        return -EBUSY;
    int error = aster_power_activity_begin();
    if (!error) {
        if (xTaskCreatePinnedToCore(run, "avatar-rig", 12288, NULL, 3, NULL, 0) == pdPASS)
            return 0;
        aster_power_activity_end();
        error = -ENOMEM;
    }
    atomic_store(&running, false);
    return error;
}
