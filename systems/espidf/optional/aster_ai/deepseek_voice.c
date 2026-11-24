/* SPDX-License-Identifier: MIT */
#include "deepseek_voice.h"
#include "speech_clause.h"
#include "ai_credentials.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct deepseek_voice {
    esp_websocket_client_handle_t ws;
    deepseek_turn_t *api;
    struct deepseek_voice *previous;
    atomic_bool connected, finished;
    atomic_int error;
    size_t sent, received, control_len;
    int opcode;
    char control[1024];
    bool attempted, sealed;
};

static int wait_finish(struct deepseek_voice *v)
{
    int64_t deadline = esp_timer_get_time() + 120000000;
    while (!atomic_load(&v->finished) && !atomic_load(&v->error) && !v->api->cancelled() &&
           esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    int rc = atomic_load(&v->error);
    return v->api->cancelled() ? -ECANCELED : rc ? rc : atomic_load(&v->finished) ? 0 : -ETIMEDOUT;
}

static void ws_event(void *ctx, esp_event_base_t base, int32_t id, void *payload)
{
    (void)base;
    struct deepseek_voice *v = ctx;
    const esp_websocket_event_data_t *e = payload;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        atomic_store(&v->connected, true);
        return;
    }
    if (atomic_load(&v->finished) || atomic_load(&v->error))
        return;
    if (id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_DISCONNECTED ||
        id == WEBSOCKET_EVENT_CLOSED) {
        atomic_store(&v->error, -ECONNRESET);
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA || e->op_code >= 8)
        return;
    if (e->payload_offset == 0 && e->op_code != 0) {
        v->opcode = e->op_code;
        v->control_len = 0;
    }
    size_t n = e->data_len;
    int rc = 0;
    if (v->opcode == 2 && n) {
        if (!v->received && v->previous)
            rc = wait_finish(v->previous);
        if (!rc && (!v->api->receive_pcm || n > 48000u * 120u - v->received))
            rc = -EPROTO;
        if (!rc) {
            if (!v->received)
                ESP_LOGI("aster_ai", "stream first audio");
            rc = v->api->receive_pcm(v->api->output, (const uint8_t *)e->data_ptr, n);
            v->received += n;
        }
    } else if (v->opcode == 1) {
        if (n >= sizeof(v->control) - v->control_len) {
            rc = -EPROTO;
        } else {
            if (n)
                memcpy(v->control + v->control_len, e->data_ptr, n);
            v->control_len += n;
            if (e->fin && e->payload_offset + e->data_len == e->payload_len) {
                v->control[v->control_len] = 0;
                cJSON *msg = cJSON_Parse(v->control);
                const char *event = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "event"));
                const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "code"));
                if (!event)
                    rc = -EPROTO;
                else if (!strcmp(event, "finish")) {
                    if (code && !strcmp(code, "success") && v->received && !(v->received % 2)) {
                        rc = v->api->flush_pcm(v->api->output);
                        if (!rc)
                            atomic_store(&v->finished, true);
                    } else
                        rc = -EPROTO;
                }
                cJSON_Delete(msg);
            }
        }
    }
    if (rc)
        atomic_store(&v->error, rc);
}

static struct deepseek_voice *begin(deepseek_turn_t *api, struct deepseek_voice *previous)
{
    struct deepseek_voice *v = heap_caps_calloc(1, sizeof(*v), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!v)
        return NULL;
    char header[AI_KEY_CAP + 32];
    snprintf(header, sizeof(header), "Authorization: Bearer %s\r\n", api->key);
    const esp_websocket_client_config_t config = {
        .uri = "wss://api.deepseek.com/v1/tools/tts?format=pcm&voice_id=stella",
        .crt_bundle_attach = esp_crt_bundle_attach, .headers = header,
        .disable_auto_reconnect = true, .buffer_size = 2048, .task_stack = 7168,
        .network_timeout_ms = 6500,
    };
    v->api = api;
    v->previous = previous;
    v->ws = esp_websocket_client_init(&config);
    ai_secret_clear(header, sizeof(header));
    if (v->ws && esp_websocket_register_events(v->ws, WEBSOCKET_EVENT_ANY, ws_event, v) == ESP_OK &&
        esp_websocket_client_start(v->ws) == ESP_OK)
        return v;
    if (v->ws)
        esp_websocket_client_destroy(v->ws);
    free(v);
    return NULL;
}

struct deepseek_voice *deepseek_voice_begin(deepseek_turn_t *api)
{
    return begin(api, NULL);
}

static void seal(struct deepseek_voice *v)
{
    static const char end[] = "{\"event\":\"finish_text\"}";
    if (!v->sealed && !atomic_load(&v->error)) {
        int n = sizeof(end) - 1;
        if (esp_websocket_client_send_text(v->ws, end, n, pdMS_TO_TICKS(2500)) != n)
            atomic_store(&v->error, -EIO);
        else
            v->sealed = true;
    }
}

static void send_pending(struct deepseek_voice *v, const char *text, size_t length)
{
    if (length == v->sent)
        return;
    char *piece = strndup(text + v->sent, length - v->sent);
    cJSON *obj = cJSON_CreateObject();
    char *wire = cJSON_AddStringToObject(obj, "event", "text") &&
                 piece && cJSON_AddStringToObject(obj, "text", piece) ? cJSON_PrintUnformatted(obj) : NULL;
    free(piece);
    cJSON_Delete(obj);
    if (!wire) {
        atomic_store(&v->error, -ENOMEM);
        return;
    }
    int n = (int)strlen(wire);
    v->attempted = true;
    if (esp_websocket_client_send_text(v->ws, wire, n, pdMS_TO_TICKS(2500)) != n)
        atomic_store(&v->error, -EIO);
    else {
        if (!v->sent)
            ESP_LOGI("aster_ai", "stream first text sent chars_bytes=%u", (unsigned)length);
        v->sent = length;
    }
    cJSON_free(wire);
}

void deepseek_voice_delta(struct deepseek_voice *v, const char *text, size_t length)
{
    if (!v || v->sealed || atomic_load(&v->error) || !atomic_load(&v->connected) || v->api->cancelled())
        return;
    size_t boundary = v->api->single_utterance ? 0 : speech_first_clause(text, length);
    if (boundary) {
        /* A final token can contain both the punctuation and the start of the
         * next sentence. Never send that suffix as part of both utterances. */
        if (boundary < v->sent)
            boundary = v->sent;
        send_pending(v, text, boundary);
        seal(v);
        ESP_LOGI("aster_ai", "stream first clause committed bytes=%u", (unsigned)v->sent);
        return;
    }
    const char *new_text = text + v->sent;
    size_t bytes = length - v->sent;
    if (bytes >= (v->sent ? 42u : 18u) || strpbrk(new_text, ".!?,;:\n") ||
        strstr(new_text, "。") || strstr(new_text, "，") || strstr(new_text, "！") || strstr(new_text, "？"))
        send_pending(v, text, length);
}

static bool ready(struct deepseek_voice *v, deepseek_turn_t *api)
{
    int64_t deadline = esp_timer_get_time() + 6500000;
    while (v && !atomic_load(&v->connected) && !atomic_load(&v->error) && !api->cancelled() &&
           esp_timer_get_time() < deadline)
        vTaskDelay(1);
    return v && atomic_load(&v->connected) && !atomic_load(&v->error) && !api->cancelled();
}

static void release_socket(struct deepseek_voice *v)
{
    if (v && v->ws) {
        esp_websocket_client_stop(v->ws);
        esp_websocket_client_destroy(v->ws);
        v->ws = NULL;
    }
}

int deepseek_voice_finish(struct deepseek_voice *v, deepseek_turn_t *api, const char *answer)
{
    if (!ready(v, api)) {
        if (api->cancelled())
            return -ECANCELED;
        if (v && v->attempted)
            return atomic_load(&v->error) ? atomic_load(&v->error) : -EIO;
        ESP_LOGW("aster_ai", "TTS socket unavailable before first text, using HTTPS");
        return deepseek_tts(api, answer);
    }
    const char *remaining = answer + v->sent;
    struct deepseek_voice *tail = NULL;
    int rc;
    if (v->sealed && speech_has_words(remaining)) {
        /* AES record decryption needs contiguous internal DMA memory. On a
         * tight heap, retire the first socket while its PCM is still playing. */
        size_t dma = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (dma < 48 * 1024) {
            ESP_LOGI("aster_ai", "serializing speech sockets DMA largest=%u", (unsigned)dma);
            rc = wait_finish(v);
            release_socket(v);
            if (rc)
                return rc;
        }
        tail = begin(api, v);
        if (ready(tail, api)) {
            send_pending(tail, remaining, strlen(remaining));
            seal(tail);
            rc = wait_finish(tail);
        } else {
            rc = wait_finish(v);
            release_socket(tail);
            release_socket(v);
            if (!rc && !api->cancelled())
                rc = deepseek_tts(api, remaining);
        }
    } else {
        if (!v->sealed) {
            send_pending(v, answer, strlen(answer));
            seal(v);
        }
        rc = wait_finish(v);
    }
    if (!rc)
        rc = api->end_pcm(api->output);
    deepseek_voice_close(tail);
    return rc;
}

void deepseek_voice_close(struct deepseek_voice *v)
{
    if (v) {
        release_socket(v);
        free(v);
    }
}
