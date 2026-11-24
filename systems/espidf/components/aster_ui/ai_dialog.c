/* SPDX-License-Identifier: MIT */
#include "ai_dialog.h"
#include "ai_endpoint.h"
#include "aster_vad.h"
#include "ai_credentials.h"
#include "ai_tts_stream.h"
#include "deepseek_voice.h"
#include "xfyun_asr.h"
#include "volc_tts.h"
#include "clock.h"
#include "aster_device.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static atomic_int phase;
static atomic_uint revision;
static atomic_bool cancelled, submitted;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static struct dialog_view view;

#if CONFIG_IDF_TARGET_ESP32S31
/* During recognition, keep the ASR socket as the only active TLS client.
 * Concurrent handshakes or synthesis can exhaust its AES DMA pool. */
static const bool overlap_asr_with_reply = false;
#else
static const bool overlap_asr_with_reply = true;
#endif

struct chat_prewarm {
    deepseek_turn_t api;
    atomic_uint references;
    atomic_bool ready;
    char key[AI_KEY_CAP];
};

struct session {
    enum ai_query_source source;
    deepseek_turn_t api;
    struct deepseek_voice *voice;
    struct volc_tts *volc;
    struct chat_prewarm *prewarm;
    struct ai_speech_keys speech_keys;
    struct ai_reply_sink sink;
    char key[AI_KEY_CAP];
    struct dialog_view current;
    struct deepseek_exchange history[4];
    size_t history_count;
};

enum dialog_phase ai_dialog_phase(void)
{
    return atomic_load(&phase);
}

unsigned ai_dialog_revision(void)
{
    return atomic_load(&revision);
}

const char *ai_dialog_name(enum dialog_phase state)
{
    static const char *const names[] = {"idle", "starting", "recording", "followup", "speech", "wifi", "asr", "chat",
        "tts", "speaking", "cancelling", "done", "failed"};
    return names[state];
}

void ai_dialog_snapshot(struct dialog_view *out)
{
    portENTER_CRITICAL(&guard);
    *out = view;
    portEXIT_CRITICAL(&guard);
}

static bool stopping(void)
{
    return atomic_load(&cancelled);
}

static void release_prewarm(struct chat_prewarm *p)
{
    if (atomic_fetch_sub(&p->references, 1) == 1) {
        deepseek_close(&p->api);
        ai_secret_clear(p->key, sizeof(p->key));
        free(p);
    }
}

static void prewarm_task(void *context)
{
    struct chat_prewarm *p = context;
    int rc = deepseek_warm_chat(&p->api);
    ESP_LOGI("aster_ai", "chat prewarm complete rc=%d", rc);
    atomic_store(&p->ready, !rc);
    release_prewarm(p);
    vTaskDelete(NULL);
}

static void start_prewarm(struct session *s)
{
    if (s->api.connection || s->prewarm)
        return;
    struct chat_prewarm *p = calloc(1, sizeof(*p));
    if (!p)
        return;
    memcpy(p->key, s->key, sizeof(p->key));
    p->api.key = p->key;
    atomic_init(&p->references, 2);
    atomic_init(&p->ready, false);
    if (xTaskCreate(prewarm_task, "ai_chat_warm", 8192, p, 3, NULL) == pdPASS) {
        s->prewarm = p;
        ESP_LOGI("aster_ai", "chat prewarm started during capture");
    } else {
        /* The worker never took its reference. */
        atomic_store(&p->references, 1);
        release_prewarm(p);
    }
}

static void finish_prewarm(struct session *s, bool use)
{
    struct chat_prewarm *p = s->prewarm;
    if (!p)
        return;
    s->prewarm = NULL;
    if (use && !s->api.connection && atomic_load(&p->ready)) {
        s->api.connection = p->api.connection;
        p->api.connection = NULL;
        ESP_LOGI("aster_ai", "chat prewarm reused");
    }
    release_prewarm(p);
}

static void publish(enum dialog_phase next, const struct dialog_view *updated)
{
    portENTER_CRITICAL(&guard);
    if (!stopping()) {
        if (updated)
            view = *updated;
        atomic_store(&phase, next);
    }
    portEXIT_CRITICAL(&guard);
    ESP_LOGI("aster_ai", "voice phase=%s", ai_dialog_name(next));
}

static int record_question(struct session *s, int16_t *mono, size_t *samples, bool followup,
                           struct xfyun_asr *asr, int *stream_error)
{
    int frame_size = aster_vad_open(asr ? 416 : 672);
    if (frame_size < 0)
        return frame_size;
    size_t block = (size_t)frame_size;
    int16_t *frame = NULL;
    aster_audio_info_t info = {0};
    int mic = aster_device_audio_capture_open(&info);
    int rc = -ENODATA;
    if (mic < 0) {
        rc = mic;
        goto close;
    }
    if (info.sample_rate != AI_MIC_RATE || info.bits != 16 || (info.channels != 1 && info.channels != 2)) {
        rc = -ENOTSUP;
        goto close;
    }
    size_t used = 0, bytes = block * info.channels * sizeof(int16_t);
    frame = malloc(bytes);
    if (!frame) {
        rc = -ENOMEM;
        goto close;
    }
    struct ai_endpoint gate = {0};
    int64_t deadline = esp_timer_get_time() + 28000000;
    ESP_LOGI("aster_ai", "vadnet1_medium ready frame=%u", (unsigned)block);
    publish(followup ? DIALOG_FOLLOWUP : DIALOG_RECORDING, NULL);
    while (*samples + block <= AI_TURN_LIMIT && !stopping() && esp_timer_get_time() < deadline) {
        if (atomic_load(&submitted))
            break;
        int n = aster_device_audio_capture_read(mic, (uint8_t *)frame + used, bytes - used);
        if (!n || n == -EAGAIN) {
            vTaskDelay(1);
            continue;
        }
        if (n < 0) {
            rc = n;
            goto close;
        }
        used += n;
        if (used < bytes)
            continue;
        used = 0;
        for (size_t i = 0; i < block; ++i)
            frame[i] = frame[i * info.channels];
        memcpy(mono + *samples, frame, block * sizeof(*frame));
        *samples += block;
        gate.captured += block;
        int speech = aster_vad_detect(frame);
        if (speech < 0) {
            rc = speech;
            goto close;
        }
        if (speech && !gate.heard) {
            gate.heard = true;
            size_t skip = ai_endpoint_leading_skip(*samples);
            if (skip) {
                *samples -= skip;
                memmove(mono, mono + skip, *samples * sizeof(*mono));
            }
            publish(DIALOG_VOICED, NULL);
            if (asr && overlap_asr_with_reply)
                start_prewarm(s);
        }
        if (asr && gate.heard && !*stream_error) {
            *stream_error = xfyun_asr_feed(asr, mono, *samples);
            if (*stream_error)
                ESP_LOGW("aster_ai", "xfyun upload interrupted, continuing capture rc=%d", *stream_error);
        }
        bool finished = asr && gate.heard ? !speech : ai_endpoint_finished(&gate, speech);
        if (finished) {
            /* VAD has already buffered the end silence. Keep a short tail. */
            if (!asr && gate.heard && *samples > AI_MIC_RATE / 2)
                *samples -= AI_MIC_RATE / 3;
            break;
        }
    }
    ESP_LOGI("aster_ai", "vad end heard=%d captured_ms=%u kept_ms=%u", gate.heard,
             (unsigned)(gate.captured / 16), (unsigned)(*samples / 16));
    rc = gate.heard ? 0 : -ENODATA;
close:
    if (mic >= 0)
        aster_device_audio_capture_close(mic);
    free(frame);
    aster_vad_close();
    return stopping() ? -ECANCELED : rc;
}

static void remember(struct session *s)
{
    size_t capacity = sizeof(s->history) / sizeof(s->history[0]);
    if (s->history_count == capacity) {
        memmove(s->history, s->history + 1, (capacity - 1) * sizeof(s->history[0]));
        --s->history_count;
    }
    struct deepseek_exchange *item = &s->history[s->history_count++];
    memcpy(item->question, s->current.question, sizeof(item->question));
    memcpy(item->answer, s->current.answer, sizeof(item->answer));
}

static int wait_for_network(void)
{
    aster_wifi_status_t wifi;
    bool requested = false, announced = false;
    int64_t deadline = esp_timer_get_time() + 35000000;
    while (!stopping() && esp_timer_get_time() < deadline) {
        if (aster_device_wifi_status(&wifi) || wifi.state == ASTER_PLAT_WIFI_STATE_DISABLED)
            break;
        if (wifi.state == ASTER_PLAT_WIFI_STATE_COMPLETED && wifi.ip[0])
            return 0;
        if (!announced) {
            publish(DIALOG_NETWORK, NULL);
            announced = true;
        }
        uint32_t op = 0, last = 0;
        int result = 0;
        aster_device_wifi_operation(&op, &last, &result);
        if (!op) {
            if (requested)
                break;
            int rc = aster_device_wifi_reconnect_start();
            if (rc && rc != -EBUSY)
                break;
            requested = !rc;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return stopping() ? -ECANCELED : -ENETDOWN;
}

static void speaking(void)
{
    publish(DIALOG_SPEAKING, NULL);
}

static int update_answer(void *ctx, size_t length)
{
    struct session *s = ctx;
    if (stopping())
        return -ECANCELED;
    bool first = ai_dialog_phase() == DIALOG_THINKING;
    portENTER_CRITICAL(&guard);
    if (!stopping()) {
        memcpy(view.answer, s->current.answer, length + 1);
        if (first)
            atomic_store(&phase, DIALOG_SYNTH);
        atomic_fetch_add(&revision, 1);
    }
    portEXIT_CRITICAL(&guard);
    if (first)
        ESP_LOGI("aster_ai", "stream first token");
    if (s->volc)
        return volc_tts_delta(s->volc, s->current.answer, length);
    deepseek_voice_delta(s->voice, s->current.answer, length);
    return 0;
}

static int answer(void *ctx)
{
    struct session *s = ctx;
    finish_prewarm(s, !stopping());
    publish(DIALOG_THINKING, &s->current);
    bool reused = s->api.connection != NULL;
    int rc = deepseek_chat(&s->api, s->current.question, s->history, s->history_count,
                           s->current.answer, update_answer, s);
    if (s->volc && reused && rc == -EIO && !s->api.connection &&
        s->api.http_status <= 0 && !s->current.answer[0] && !stopping())
        rc = deepseek_chat(&s->api, s->current.question, s->history, s->history_count,
                           s->current.answer, update_answer, s);
    if (!s->volc)
        deepseek_close(&s->api);
    if (!rc && !stopping()) {
        ESP_LOGI("aster_ai", "stream answer complete bytes=%u", (unsigned)strlen(s->current.answer));
        rc = s->volc ? volc_tts_finish(s->volc, s->current.answer) :
                       deepseek_voice_finish(s->voice, &s->api, s->current.answer);
    }
    /* Join WebSocket callbacks before the caller releases their PCM queue. */
    deepseek_voice_close(s->voice);
    s->voice = NULL;
    volc_tts_close(s->volc);
    s->volc = NULL;
    return rc;
}

static void converse(void *arg)
{
    struct session *s = arg;
    int rc = ai_key_read(s->key);
    bool speech_configured = ai_speech_keys_read(&s->speech_keys) == 0;
    bool stream_asr = speech_configured && s->source == AI_MICROPHONE;
    ESP_LOGI("aster_ai", "voice providers asr=%s llm=deepseek tts=%s",
             stream_asr ? "xfyun_v2" : s->source == AI_MICROPHONE ? "deepseek" : "text",
             speech_configured ? "volc_v2" : "deepseek");
    s->api = (deepseek_turn_t){.key = s->key, .cancelled = stopping,
                               .single_utterance = s->source == AI_BENCH_WHOLE};
    unsigned completed = 0;
    size_t samples = 0;
    while (!rc && !stopping()) {
        samples = 0;
        s->api.http_status = 0;
        atomic_store(&submitted, false);
        int16_t *mono = NULL;
        struct xfyun_asr *asr = NULL;
        int stream_error = 0;
        if (stream_asr) {
            rc = wait_for_network();
            if (!rc && !aster_idf_clock_synced()) {
                publish(DIALOG_NETWORK, NULL);
                int64_t deadline = esp_timer_get_time() + 8000000;
                while (!stopping() && !aster_idf_clock_synced() && esp_timer_get_time() < deadline)
                    vTaskDelay(pdMS_TO_TICKS(50));
                if (!aster_idf_clock_synced() && !stopping()) {
                    ESP_LOGW("aster_ai", "NTP not synchronized before ASR");
                    rc = -ETIME;
                }
            }
            if (!rc && !stopping())
                rc = xfyun_asr_start(&asr, &s->speech_keys);
        }
        if (!rc && !stopping() && s->source == AI_MICROPHONE) {
            mono = heap_caps_malloc(AI_TURN_LIMIT * sizeof(*mono), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            rc = mono ? record_question(s, mono, &samples, completed != 0, asr, &stream_error) : -ENOMEM;
        }
        if (rc == -ENODATA && completed) {
            /* A quiet follow-up closes normally and retains the last answer on screen. */
            xfyun_asr_close(asr);
            rc = 0;
            free(mono);
            break;
        }
        if (!rc && !stopping())
            memset(&s->current, 0, sizeof(s->current));
        if (asr && !rc && !stopping()) {
            publish(DIALOG_ASR, &s->current);
            if (!stream_error)
                xfyun_asr_seal(asr, samples);
            rc = ai_reply_sink_open(&s->sink);
            if (!rc && !stream_error && overlap_asr_with_reply)
                rc = volc_tts_open(&s->volc, &s->api, &s->speech_keys);
            if (!rc)
                rc = xfyun_asr_transcribe(&asr, &s->speech_keys, stream_error, mono, samples,
                                         s->current.question, sizeof(s->current.question), stopping);
        }
        if (asr && !rc && overlap_asr_with_reply)
            xfyun_asr_close_async(asr);
        else
            xfyun_asr_close(asr);
        if (!rc && !stopping()) {
            rc = wait_for_network();
        }
        if (!rc && !stopping() && !stream_asr) {
            /* Reserve I2S/DMA before TLS and Wi-Fi compete for internal SRAM. */
            rc = ai_reply_sink_open(&s->sink);
            ESP_LOGI("aster_ai", "audio reserve rc=%d DMA largest=%u",
                     rc, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        }
        if (!rc && !stopping() && s->source == AI_BENCH_WARM)
            rc = deepseek_warm_chat(&s->api);
        if (!rc && !stopping()) {
            if (speech_configured && !s->volc)
                rc = volc_tts_open(&s->volc, &s->api, &s->speech_keys);
            else if (!speech_configured)
                s->voice = deepseek_voice_begin(&s->api);
            if (!rc && s->source == AI_MICROPHONE && !stream_asr) {
                publish(DIALOG_ASR, &s->current);
                rc = deepseek_asr(&s->api, mono, samples, s->current.question);
            } else if (!rc && s->source != AI_MICROPHONE) {
                strcpy(s->current.question, "请用五个完整的句子，用中文讲一个小鲸寻找月亮的小故事。");
                ESP_LOGI("aster_ai", "bench mode=%s", s->source == AI_BENCH_WARM ? "warm" :
                         s->api.single_utterance ? "whole" : "clause");
            }
        }
        free(mono);
        if (!rc && !stopping()) {
            /* The first few PCM chunks can arrive at irregular intervals.
             * Short replies start as soon as the server completes them. */
            rc = ai_tts_stream(&s->api, &s->sink, s->volc ? 4 : 1, answer, s, speaking);
        }
        deepseek_voice_close(s->voice);
        s->voice = NULL;
        volc_tts_close(s->volc);
        s->volc = NULL;
        if (!stream_asr)
            deepseek_close(&s->api);
        int closed = ai_reply_sink_close(&s->sink);
        if (!rc)
            rc = closed;
        if (rc == -ENODATA && stream_asr && completed) {
            const struct deepseek_exchange *last = &s->history[s->history_count - 1];
            memcpy(s->current.question, last->question, sizeof(s->current.question));
            memcpy(s->current.answer, last->answer, sizeof(s->current.answer));
            rc = 0;
            break;
        }
        if (rc || stopping())
            break;
        remember(s);
        ++completed;
        ESP_LOGI("aster_ai", "turn=%u http=%d kept_ms=%u tls_connections=%u", completed,
                 s->api.http_status, (unsigned)(samples / 16), s->api.connections);
        s->api.connections = 0;
        if (s->source != AI_MICROPHONE)
            break;
        /* Let the local speaker tail decay before admitting another utterance. */
        vTaskDelay(pdMS_TO_TICKS(220));
    }
    s->current.error = rc;
    s->current.http = s->api.http_status;
    s->current.failed_phase = ai_dialog_phase();
    finish_prewarm(s, false);
    deepseek_close(&s->api);
    ai_secret_clear(s->key, sizeof(s->key));
    ai_secret_clear(&s->speech_keys, sizeof(s->speech_keys));
    portENTER_CRITICAL(&guard);
    bool reset = stopping();
    if (!reset)
        view = s->current;
    atomic_store(&phase, reset ? DIALOG_IDLE : rc ? DIALOG_FAILED : DIALOG_DONE);
    portEXIT_CRITICAL(&guard);
    ESP_LOGI("aster_ai", "voice end result=%d stage=%s http=%d duration_ms=%u", reset ? -ECANCELED : rc,
             ai_dialog_name(s->current.failed_phase), s->api.http_status, (unsigned)(samples / 16));
    free(s);
    vTaskDelete(NULL);
}

/* Called only from the Launcher thread. */
void ai_dialog_start(enum ai_query_source source)
{
    enum dialog_phase state = ai_dialog_phase();
    if (state != DIALOG_IDLE && state != DIALOG_DONE && state != DIALOG_FAILED)
        return;
    struct session *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s)
        s->source = source;
    memset(&view, 0, sizeof(view));
    atomic_store(&cancelled, false);
    atomic_store(&submitted, false);
    atomic_store(&phase, DIALOG_STARTING);
    if (!s || xTaskCreate(converse, "ai_dialog", 16384, s, 4, NULL) != pdPASS) {
        free(s);
        view.error = -ENOMEM;
        atomic_store(&phase, DIALOG_FAILED);
    }
}

void ai_dialog_submit(void)
{
    atomic_store(&submitted, true);
}

void ai_dialog_reset(void)
{
    portENTER_CRITICAL(&guard);
    int state = ai_dialog_phase();
    if (state != DIALOG_IDLE && state != DIALOG_CANCELLING) {
        atomic_store(&cancelled, true);
        memset(&view, 0, sizeof(view));
        atomic_store(&phase, state == DIALOG_DONE || state == DIALOG_FAILED ? DIALOG_IDLE :
                     DIALOG_CANCELLING);
    }
    portEXIT_CRITICAL(&guard);
}
