/* SPDX-License-Identifier: MIT */
#include "ai_reply_audio.h"
#include "aster_audio_output.h"
#ifdef ASTER_IDF_MEDIA_PLAYER
#include "aster_media.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { OUTPUT_FRAMES = 512, MAX_BYTES = 24000 * 2 * 120 };

static atomic_uint mouth_until_ms;
static atomic_uint mouth_level;

unsigned ai_reply_audio_level(void)
{
    return ai_reply_audio_voicing() ? atomic_load(&mouth_level) : 0;
}

bool ai_reply_audio_voicing(void)
{
    uint32_t until = atomic_load(&mouth_until_ms);
    return until && (int32_t)(until - (uint32_t)(esp_timer_get_time() / 1000)) > 0;
}

void ai_reply_audio_clear(struct ai_reply_audio *audio)
{
    while (audio->first) {
        struct ai_pcm_block *next = audio->first->next;
        free(audio->first);
        audio->first = next;
    }
    *audio = (struct ai_reply_audio){0};
}

int ai_reply_audio_append(void *context, const uint8_t *data, size_t bytes)
{
    struct ai_reply_audio *audio = context;
    if (bytes > MAX_BYTES - audio->bytes)
        return -EFBIG;
    while (bytes) {
        if (!audio->last || audio->last->used == AI_PCM_BLOCK_BYTES) {
            struct ai_pcm_block *block = heap_caps_malloc(sizeof(*block), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!block)
                return -ENOMEM;
            block->next = NULL;
            block->used = 0;
            if (audio->last)
                audio->last->next = block;
            else
                audio->first = block;
            audio->last = block;
        }
        size_t n = AI_PCM_BLOCK_BYTES - audio->last->used;
        if (n > bytes)
            n = bytes;
        memcpy(audio->last->pcm + audio->last->used, data, n);
        audio->last->used += n;
        audio->bytes += n;
        data += n;
        bytes -= n;
    }
    return 0;
}

static int write_all(uint32_t endpoint, const int32_t *stereo, uint32_t count,
                     uint32_t limit, bool (*cancelled)(void))
{
    int64_t deadline = esp_timer_get_time() + 5000000;
    while (count) {
        if (cancelled())
            return -ECANCELED;
        int n = aster_audio_output_write(endpoint, stereo, count < limit ? count : limit);
        if (n == -EAGAIN) {
            if (esp_timer_get_time() >= deadline)
                return -ETIMEDOUT;
            vTaskDelay(1);
        } else if (n < 0) {
            return n;
        } else {
            stereo += n * 2;
            count -= n;
        }
    }
    return 0;
}

int ai_reply_sink_open(struct ai_reply_sink *sink)
{
#ifdef ASTER_IDF_MEDIA_PLAYER
    uint32_t endpoint = aster_media_default_output();
#else
    uint32_t endpoint = ASTER_AUDIO_OUTPUT_WIRED;
#endif
    const aster_audio_format_t fmt = {.size = sizeof(fmt), .version = 1, .rate = 48000,
                                      .channels = 2, .format = ASTER_AUDIO_PCM_S32LE};
    aster_audio_output_caps_t caps;
    int rc = aster_audio_output_caps(endpoint, &caps);
    if (!rc) {
        sink->stereo = heap_caps_malloc(OUTPUT_FRAMES * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!sink->stereo)
            return -ENOMEM;
    }
    if (!rc)
        rc = aster_audio_output_open(endpoint, &fmt);
    if (rc) {
        free(sink->stereo);
        sink->stereo = NULL;
        return rc;
    }
    sink->endpoint = endpoint;
    sink->max_frames = caps.max_write_frames;
    return 0;
}

int ai_reply_sink_close(struct ai_reply_sink *sink)
{
    if (!sink->stereo)
        return 0;
    int rc = aster_audio_output_close(sink->endpoint);
    free(sink->stereo);
    *sink = (struct ai_reply_sink){0};
    return rc;
}

int ai_reply_audio_play(struct ai_reply_audio *audio, struct ai_reply_sink *sink, bool (*cancelled)(void))
{
    if (cancelled())
        return -ECANCELED;
    if (!sink->stereo || !audio->first || !audio->bytes || audio->bytes % 2)
        return -EPROTO;
    int rc = 0;
    int32_t *stereo = sink->stereo;
    int previous = 0;
    bool first = true;
    int64_t start = esp_timer_get_time();
    atomic_store(&mouth_until_ms, 0);
    size_t played = 0;
    while (audio->first && !rc) {
        struct ai_pcm_block *b = audio->first;
        if (b->used % 2) {
            rc = -EPROTO;
            break;
        }
        for (size_t pos = 0; pos < b->used && !rc;) {
            size_t bytes = b->used - pos;
            if (bytes > OUTPUT_FRAMES)
                bytes = OUTPUT_FRAMES;
            /* Two input bytes yield two stereo frames at 48 kHz. */
            unsigned magnitude = 0;
            for (size_t i = 0; i < bytes; i += 2) {
                const uint8_t *p = b->pcm + pos + i;
                int sample = (int16_t)((uint16_t)p[1] << 8 | p[0]);
                magnitude += abs(sample);
                int32_t midpoint = ((first ? sample : previous) + sample) * 32768;
                stereo[i * 2] = stereo[i * 2 + 1] = midpoint;
                stereo[i * 2 + 2] = stereo[i * 2 + 3] = sample * 65536;
                previous = sample;
                first = false;
            }
            rc = write_all(sink->endpoint, stereo, bytes, sink->max_frames, cancelled);
            pos += bytes;
            if (!rc) {
                played += bytes;
                if (magnitude > 150 * bytes) {
                    unsigned level = magnitude / bytes / 15;
                    atomic_store(&mouth_level, level > 100 ? 100 : level);
                    atomic_store(&mouth_until_ms, (uint32_t)(esp_timer_get_time() / 1000) + 47);
                }
            }
        }
        audio->first = b->next;
        audio->bytes -= b->used;
        free(b);
        if (!audio->first) {
            audio->last = NULL;
            if (!rc && audio->refill)
                rc = audio->refill(audio->context, &audio->first);
            if (audio->first)
                audio->bytes = audio->first->used;
        }
    }
    if (!rc && !cancelled()) {
        rc = aster_audio_output_drain(sink->endpoint);
        if (rc == -ENOTSUP)
            rc = 0;
    }
    atomic_store(&mouth_until_ms, 0);
    ESP_LOGI("aster_ai", "reply audio bytes=%u nominal_ms=%u output_ms=%u result=%d",
             (unsigned)played, (unsigned)(played / 48),
             (unsigned)((esp_timer_get_time() - start) / 1000), rc);
    return rc;
}
