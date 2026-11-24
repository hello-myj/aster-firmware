/* SPDX-License-Identifier: MIT */
#include "xfyun_asr.h"
#include "xfyun_text.h"
#include "deepseek.h"
#include "speech_socket.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "sdkconfig.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HOST "iat.cn-huabei-1.xf-yun.com"
enum { FRAME_SAMPLES = 512, FRAME_BYTES = FRAME_SAMPLES * sizeof(int16_t) };
struct xfyun_asr {
    struct speech_socket socket;
    /* Recorder appends immutable samples to its retained utterance buffer. */
    const int16_t *input;
    SemaphoreHandle_t input_changed, sender_done;
    atomic_size_t available;
    atomic_bool sealed, abort_sender;
    atomic_int sender_result;
    bool sender_joined, sender_unavailable;
    char app_id[sizeof(((struct ai_speech_keys *)0)->app_id)];
    unsigned sequence;
    size_t sent, text_bytes;
    int64_t next_frame;
    char text[AI_QUESTION_CAP];
    char base64[4 * ((FRAME_BYTES + 2) / 3) + 1];
    char request[2048];
};

static int append_result(struct xfyun_asr *a, const char *encoded)
{
    size_t length = 0;
    size_t limit = strlen(encoded);
    unsigned char *json = heap_caps_malloc(limit + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json)
        return -ENOMEM;
    int rc = mbedtls_base64_decode(json, limit, &length, (const unsigned char *)encoded, limit);
    cJSON *decoded = rc ? NULL : cJSON_ParseWithLength((char *)json, length);
    const cJSON *words = cJSON_GetObjectItemCaseSensitive(decoded, "ws");
    rc = cJSON_IsArray(words) ? 0 : -EPROTO;
    const cJSON *word;
    cJSON_ArrayForEach(word, words) {
        const cJSON *candidate = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(word, "cw"), 0);
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(candidate, "w"));
        if (!text)
            continue;
        size_t bytes = strlen(text);
        if (bytes >= sizeof(a->text) - a->text_bytes) {
            rc = -EFBIG;
            break;
        }
        memcpy(a->text + a->text_bytes, text, bytes + 1);
        a->text_bytes += bytes;
    }
    cJSON_Delete(decoded);
    free(json);
    return rc;
}

static int message(void *context, const uint8_t *data, size_t bytes)
{
    struct xfyun_asr *a = context;
    cJSON *reply = cJSON_ParseWithLength((const char *)data, bytes + 1);
    cJSON *header = cJSON_GetObjectItemCaseSensitive(reply, "header");
    cJSON *code = cJSON_GetObjectItemCaseSensitive(header, "code");
    cJSON *status = cJSON_GetObjectItemCaseSensitive(header, "status");
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(reply, "payload");
    cJSON *result = cJSON_GetObjectItemCaseSensitive(payload, "result");
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "text"));
    int rc = -EPROTO;
    if (cJSON_IsNumber(code) && code->valueint == 0) {
        bool first = !a->text_bytes;
        rc = text && *text ? append_result(a, text) : 0;
        if (!rc && first && a->text_bytes)
            ESP_LOGI("aster_ai", "xfyun first transcript");
        if (!rc && cJSON_IsNumber(status) && status->valueint == 2)
            atomic_store(&a->socket.finished, true);
    } else if (cJSON_IsNumber(code)) {
        ESP_LOGW("aster_ai", "xfyun service error=%d", code->valueint);
        rc = -EPROTO;
    }
    cJSON_Delete(reply);
    return rc;
}

static size_t query_escape(char *out, const char *input)
{
    static const char hex[] = "0123456789ABCDEF";
    char *p = out;
    while (*input) {
        unsigned char ch = *input++;
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_' || ch == '.') {
            *p++ = ch;
        } else {
            *p++ = '%';
            *p++ = hex[ch >> 4];
            *p++ = hex[ch & 15];
        }
    }
    *p = 0;
    return p - out;
}

int xfyun_asr_start(struct xfyun_asr **out, const struct ai_speech_keys *keys)
{
    *out = NULL;
    time_t now = time(NULL);
    struct tm utc;
    char date[40];
    if (now < 1700000000 || !gmtime_r(&now, &utc) ||
        !strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &utc))
        return -EAGAIN;
    ESP_LOGI("aster_ai", "xfyun signed request at UTC %lld", (long long)now);
    char sign[144], auth[256], uri[1500];
    unsigned char digest[32], signature[48], encoded[344];
    size_t written;
    snprintf(sign, sizeof(sign), "host: " HOST "\ndate: %s\nGET /v1 HTTP/1.1", date);
    int rc = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                            (const unsigned char *)keys->api_secret, strlen(keys->api_secret),
                            (unsigned char *)sign, strlen(sign), digest);
    if (!rc)
        rc = mbedtls_base64_encode(signature, sizeof(signature), &written, digest, sizeof(digest));
    if (!rc) {
        int n = snprintf(auth, sizeof(auth),
                         "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
                         keys->api_key, (const char *)signature);
        rc = n < 0 || n >= sizeof(auth) ? -EINVAL :
             mbedtls_base64_encode(encoded, sizeof(encoded), &written, (unsigned char *)auth, n);
    }
    if (!rc) {
        int n = snprintf(uri, sizeof(uri), "wss://" HOST "/v1?host=" HOST "&date=");
        char *p = uri + n;
        p += query_escape(p, date);
        memcpy(p, "&authorization=", sizeof("&authorization=") - 1);
        query_escape(p + sizeof("&authorization=") - 1, (char *)encoded);
        struct xfyun_asr *a = heap_caps_calloc(1, sizeof(*a), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!a) {
            rc = -ENOMEM;
        } else {
            atomic_init(&a->available, 0);
            atomic_init(&a->sealed, false);
            atomic_init(&a->abort_sender, false);
            atomic_init(&a->sender_result, 0);
            memcpy(a->app_id, keys->app_id, sizeof(a->app_id));
            a->socket.provider = "xfyun";
            rc = speech_socket_open(&a->socket, uri, NULL, 1, 16384, message, NULL, a);
            if (!rc)
                *out = a;
            else
                free(a);
        }
    }
    ai_secret_clear(auth, sizeof(auth));
    ai_secret_clear(uri, sizeof(uri));
    ai_secret_clear(encoded, sizeof(encoded));
    ai_secret_clear(signature, sizeof(signature));
    ai_secret_clear(digest, sizeof(digest));
    return rc;
}

static int send_audio(struct xfyun_asr *a, const int16_t *data, size_t count, bool last)
{
    size_t bytes;
    a->base64[0] = 0;
    if (count && mbedtls_base64_encode((unsigned char *)a->base64, sizeof(a->base64), &bytes,
                                       (const unsigned char *)data, count * sizeof(*data)))
        return -EINVAL;
    int state = last ? 2 : a->sequence ? 1 : 0;
    const char *parameter = state == 0 ?
        "\"parameter\":{\"iat\":{\"domain\":\"spark_asr_v2\",\"language\":\"zh_cn\",\"accent\":\"mandarin\","
        "\"result\":{\"encoding\":\"utf8\",\"compress\":\"raw\",\"format\":\"json\"}}}," : "";
    int n = snprintf(a->request, sizeof(a->request),
                     "{\"header\":{\"app_id\":\"%s\",\"status\":%d},%s\"payload\":{\"audio\":"
                     "{\"encoding\":\"raw\",\"sample_rate\":16000,\"channels\":1,\"bit_depth\":16,"
                     "\"seq\":%u,\"status\":%d,\"audio\":\"%s\"}}}",
                     a->app_id, state, parameter, a->sequence + 1, state, a->base64);
    if (n < 0 || n >= sizeof(a->request))
        return -EFBIG;
    int64_t started = esp_timer_get_time();
    int rc = speech_socket_send(&a->socket, a->request, n, false);
    if (!rc) {
        if (!a->sequence)
            ESP_LOGI("aster_ai", "xfyun first audio sent during capture");
        ++a->sequence;
        a->sent += count;
        /* The frame's write time is part of its transmission interval. */
        a->next_frame = started + (a->sent < 5 * FRAME_SAMPLES ? 22000 : 26000);
    }
    return rc;
}

static int send_end(struct xfyun_asr *a)
{
    ESP_LOGI("aster_ai", "xfyun finish audio samples=%u", (unsigned)a->sent);
    return send_audio(a, NULL, 0, true);
}

static int drain_input(struct xfyun_asr *a)
{
    int64_t connect_by = esp_timer_get_time() + 5000000;
    while (!atomic_load(&a->abort_sender)) {
        int rc = atomic_load(&a->socket.error);
        if (rc)
            return rc;
        int64_t now = esp_timer_get_time();
        if (!atomic_load(&a->socket.connected)) {
            if (now >= connect_by)
                return -ETIMEDOUT;
            xSemaphoreTake(a->input_changed, pdMS_TO_TICKS(10));
            continue;
        }
        bool sealed = atomic_load(&a->sealed);
        size_t pending = atomic_load(&a->available) - a->sent;
        if (!pending && sealed)
            return send_end(a);
        if ((pending >= FRAME_SAMPLES || (pending && sealed)) && (sealed || now >= a->next_frame)) {
            size_t count = pending < FRAME_SAMPLES ? pending : FRAME_SAMPLES;
            rc = send_audio(a, a->input + a->sent, count, false);
            if (rc)
                return rc;
            continue;
        }
        /* Wake at an audio frame boundary or when a paced batch is due. */
        TickType_t delay = pdMS_TO_TICKS(32);
        if (pending >= FRAME_SAMPLES && a->next_frame > now)
            delay = pdMS_TO_TICKS((a->next_frame - now + 999) / 1000);
        xSemaphoreTake(a->input_changed, delay ? delay : 1);
    }
    return -ECANCELED;
}

static void sender_task(void *arg)
{
    struct xfyun_asr *a = arg;
    atomic_store(&a->sender_result, drain_input(a));
    xSemaphoreGive(a->sender_done);
#if CONFIG_IDF_TARGET_ESP32S31 && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

static BaseType_t launch_sender(struct xfyun_asr *a)
{
#if CONFIG_IDF_TARGET_ESP32S31 && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    /* This worker reads retained PCM and writes to an established socket.
     * Keep its stack out of the SRAM pool needed by AES and I2S DMA. */
    return xTaskCreateWithCaps(sender_task, "xfyun_sender", 5632, a, 4, NULL,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return xTaskCreate(sender_task, "xfyun_sender", 5632, a, 4, NULL);
#endif
}

static void start_sender(struct xfyun_asr *a, const int16_t *mono, size_t samples)
{
    SemaphoreHandle_t changed = xSemaphoreCreateBinary();
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    a->input = mono;
    atomic_store(&a->available, samples);
    a->input_changed = changed;
    a->sender_done = done;
    if (changed && done && launch_sender(a) == pdPASS) {
        ESP_LOGI("aster_ai", "xfyun sender active");
        return;
    }
    if (changed)
        vSemaphoreDelete(changed);
    if (done)
        vSemaphoreDelete(done);
    a->input_changed = NULL;
    a->sender_done = NULL;
    a->sender_unavailable = true;
}

int xfyun_asr_feed(struct xfyun_asr *a, const int16_t *mono, size_t samples)
{
    int rc = atomic_load(&a->socket.error);
    if (rc)
        return rc;
    if (!a->sender_done && !a->sender_unavailable)
        start_sender(a, mono, samples);
    if (a->sender_done) {
        atomic_store(&a->available, samples);
        xSemaphoreGive(a->input_changed);
        return atomic_load(&a->sender_result);
    }
    if (samples >= a->sent + FRAME_SAMPLES && esp_timer_get_time() >= a->next_frame &&
        atomic_load(&a->socket.connected))
        return send_audio(a, mono + a->sent, FRAME_SAMPLES, false);
    return 0;
}

void xfyun_asr_seal(struct xfyun_asr *a, size_t samples)
{
    if (!a->sender_done)
        return;
    atomic_store(&a->available, samples);
    atomic_store(&a->sealed, true);
    xSemaphoreGive(a->input_changed);
}

static int join_sender(struct xfyun_asr *a, bool (*cancelled)(void))
{
    if (!a->sender_joined) {
        while (xSemaphoreTake(a->sender_done, pdMS_TO_TICKS(40)) != pdTRUE) {
            if (cancelled && cancelled())
                return -ECANCELED;
        }
        a->sender_joined = true;
    }
    return atomic_load(&a->sender_result);
}

static void stop_sender(struct xfyun_asr *a)
{
    if (!a->sender_done)
        return;
    atomic_store(&a->abort_sender, true);
    xSemaphoreGive(a->input_changed);
    (void)join_sender(a, NULL);
    vSemaphoreDelete(a->sender_done);
    vSemaphoreDelete(a->input_changed);
    a->sender_done = NULL;
}

static int send_remaining(struct xfyun_asr *a, const int16_t *mono, size_t samples, bool replay,
                          bool (*cancelled)(void))
{
    int rc = speech_socket_wait(&a->socket, &a->socket.connected, cancelled, 5000);
    while (!rc && a->sent < samples && !cancelled()) {
        size_t count = samples - a->sent;
        int64_t started = esp_timer_get_time();
        rc = send_audio(a, mono + a->sent, count < FRAME_SAMPLES ? count : FRAME_SAMPLES, false);
        if (replay && !rc) {
            /* Re-send stored PCM at its capture rate, accounting for time spent writing. */
            int64_t remaining = started + 32000 - esp_timer_get_time();
            if (remaining >= 1000)
                vTaskDelay(pdMS_TO_TICKS(remaining / 1000));
        }
    }
    return !rc && !cancelled() ? send_end(a) : rc;
}

static int finish_audio(struct xfyun_asr *a, const int16_t *mono, size_t samples, bool replay,
                        char *question, size_t capacity, bool (*cancelled)(void))
{
    xfyun_asr_seal(a, samples);
    int rc = a->sender_done ? join_sender(a, cancelled) : send_remaining(a, mono, samples, replay, cancelled);
    if (!rc)
        rc = speech_socket_wait(&a->socket, &a->socket.finished, cancelled, 5500);
    if (!rc) {
        size_t clean = xfyun_strip_markers(a->text);
        if (!clean)
            rc = -ENODATA;
        else if (clean >= capacity)
            rc = -EFBIG;
        else {
            memcpy(question, a->text, clean + 1);
            ESP_LOGI("aster_ai", "xfyun final result bytes=%u removed=%u", (unsigned)clean,
                     (unsigned)(a->text_bytes - clean));
        }
    }
    return rc;
}

int xfyun_asr_transcribe(struct xfyun_asr **asr, const struct ai_speech_keys *keys, int stream_error,
                        const int16_t *mono, size_t samples, char *question, size_t capacity,
                        bool (*cancelled)(void))
{
    int rc = stream_error ? stream_error :
             finish_audio(*asr, mono, samples, false, question, capacity, cancelled);
    if (cancelled())
        return -ECANCELED;
    if (rc != -ECONNRESET && rc != -ETIMEDOUT && rc != -EPIPE && rc != -EIO)
        return rc;

    ESP_LOGW("aster_ai", "xfyun retry with buffered speech result=%d samples=%u", rc, (unsigned)samples);
    xfyun_asr_close(*asr);
    *asr = NULL;
    if (cancelled())
        return -ECANCELED;
    rc = xfyun_asr_start(asr, keys);
    if (!rc)
        rc = finish_audio(*asr, mono, samples, true, question, capacity, cancelled);
    return rc;
}

void xfyun_asr_close(struct xfyun_asr *a)
{
    if (a) {
        stop_sender(a);
        speech_socket_close(&a->socket);
        free(a);
    }
}

static void close_task(void *arg)
{
    xfyun_asr_close(arg);
    vTaskDelete(NULL);
}

void xfyun_asr_close_async(struct xfyun_asr *a)
{
    if (!a)
        return;
    /* The caller can release the retained PCM immediately after this call. */
    stop_sender(a);
    if (xTaskCreate(close_task, "xfyun_close", 4096, a, 3, NULL) != pdPASS)
        xfyun_asr_close(a);
}
