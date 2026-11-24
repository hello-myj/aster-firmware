/* SPDX-License-Identifier: MIT */
#include "ai_tts_stream.h"
#include "ai_reply_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>

enum { MIN_QUEUE_BLOCKS = 2, MAX_QUEUE_BLOCKS = 192, START_BLOCKS = 1 };
struct stream {
    struct ai_reply_sink *sink;
    QueueHandle_t queue;
    SemaphoreHandle_t joined;
    struct ai_reply_audio pending;
    atomic_bool finished;
    atomic_int output_error, network_error;
    bool (*cancelled)(void);
    void (*speaking)(void);
    unsigned peak_blocks, full_waits;
};

static int enqueue(struct stream *s)
{
    struct ai_pcm_block *b = s->pending.first;
    s->pending.first = b->next;
    if (!s->pending.first)
        s->pending.last = NULL;
    s->pending.bytes -= b->used;
    b->next = NULL;
    bool waited = false;
    while (!s->cancelled() && !atomic_load(&s->output_error)) {
        if (xQueueSend(s->queue, &b, pdMS_TO_TICKS(20)) == pdTRUE) {
            unsigned queued = uxQueueMessagesWaiting(s->queue);
            if (queued > s->peak_blocks)
                s->peak_blocks = queued;
            return 0;
        }
        if (!waited) {
            ++s->full_waits;
            waited = true;
        }
    }
    free(b);
    return s->cancelled() ? -ECANCELED : atomic_load(&s->output_error);
}

static int receive_pcm(void *ctx, const uint8_t *data, size_t len)
{
    struct stream *s = ctx;
    int rc = ai_reply_audio_append(&s->pending, data, len);
    while (!rc && s->pending.first && s->pending.first->used == AI_PCM_BLOCK_BYTES)
        rc = enqueue(s);
    return rc;
}

static int next_block(void *ctx, struct ai_pcm_block **block)
{
    struct stream *s = ctx;
    *block = NULL;
    while (!s->cancelled()) {
        int error = atomic_load(&s->network_error);
        if (error)
            return error;
        bool finished = atomic_load(&s->finished);
        if (xQueueReceive(s->queue, block, finished ? 0 : pdMS_TO_TICKS(10)) == pdTRUE)
            return 0;
        if (finished)
            return 0;
    }
    return -ECANCELED;
}

static int end_pcm(void *ctx)
{
    struct stream *s = ctx;
    int rc = s->pending.first ? enqueue(s) : 0;
    if (rc)
        atomic_store(&s->network_error, rc);
    atomic_store(&s->finished, true);
    return rc;
}

static int flush_pcm(void *ctx)
{
    struct stream *s = ctx;
    return s->pending.first ? enqueue(s) : 0;
}

static void speaker(void *ctx)
{
    struct stream *s = ctx;
    struct ai_reply_audio audio = {.refill = next_block, .context = s};
    int rc = 0;
    for (unsigned i = 0; i < START_BLOCKS; ++i) {
        struct ai_pcm_block *b = NULL;
        rc = next_block(s, &b);
        if (rc || !b)
            break;
        if (audio.last)
            audio.last->next = b;
        else
            audio.first = b;
        audio.last = b;
        audio.bytes += b->used;
    }
    if (!rc && audio.first && !s->cancelled()) {
        s->speaking();
        rc = ai_reply_audio_play(&audio, s->sink, s->cancelled);
    }
    ai_reply_audio_clear(&audio);
    atomic_store(&s->output_error, rc);
    xSemaphoreGive(s->joined);
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

static bool start_speaker(struct stream *s)
{
    /* This worker only consumes PCM. The internal-stack dialogue task owns
     * codec setup, teardown, credentials and all HTTPS/TLS operations. */
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    return xTaskCreateWithCaps(speaker, "ai_speaker", 6144, s, 6, NULL,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
#else
    return xTaskCreate(speaker, "ai_speaker", 6144, s, 6, NULL) == pdPASS;
#endif
}

int ai_tts_stream(deepseek_turn_t *api, struct ai_reply_sink *sink, int (*produce)(void *), void *context,
                  void (*on_speaking)(void))
{
    struct stream s = {.sink = sink, .cancelled = api->cancelled, .speaking = on_speaking};
    /* Only the pointer slots are reserved. A fast server may send its entire
     * answer in a burst and immediately close, so keep that burst in PSRAM. */
    size_t slots = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (4 * sizeof(struct ai_pcm_block));
    if (slots > MAX_QUEUE_BLOCKS)
        slots = MAX_QUEUE_BLOCKS;
    if (slots < MIN_QUEUE_BLOCKS)
        return -ENOMEM;
    s.queue = xQueueCreate(slots, sizeof(struct ai_pcm_block *));
    s.joined = xSemaphoreCreateBinary();
    int rc = -ENOMEM;
    if (s.queue && s.joined && start_speaker(&s)) {
        api->receive_pcm = receive_pcm;
        api->end_pcm = end_pcm;
        api->flush_pcm = flush_pcm;
        api->output = &s;
        rc = produce(context);
        api->receive_pcm = NULL;
        api->end_pcm = NULL;
        api->flush_pcm = NULL;
        api->output = NULL;
        if (!rc && !atomic_load(&s.finished))
            rc = end_pcm(&s);
        atomic_store(&s.network_error, rc);
        atomic_store(&s.finished, true);
        xSemaphoreTake(s.joined, portMAX_DELAY);
        if (!rc)
            rc = atomic_load(&s.output_error);
    }
    ai_reply_audio_clear(&s.pending);
    if (s.queue) {
        struct ai_pcm_block *b;
        while (xQueueReceive(s.queue, &b, 0) == pdTRUE)
            free(b);
        vQueueDelete(s.queue);
    }
    if (s.joined)
        vSemaphoreDelete(s.joined);
    ESP_LOGI("aster_ai", "PCM queue peak=%u blocks capacity=%u full_waits=%u",
             s.peak_blocks, (unsigned)slots, s.full_waits);
    return rc;
}
