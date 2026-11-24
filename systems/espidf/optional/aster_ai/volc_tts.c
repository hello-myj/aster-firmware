/* SPDX-License-Identifier: MIT */
#include "volc_tts.h"
#include "speech_socket.h"
#include "speech_clause.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    START_CONNECTION = 1, FINISH_CONNECTION = 2, CONNECTION_STARTED = 50,
    CONNECTION_FAILED = 51, START_SESSION = 100, FINISH_SESSION = 102,
    SESSION_STARTED = 150, SESSION_CANCELED = 151, SESSION_FINISHED = 152,
    SESSION_FAILED = 153, TASK_REQUEST = 200, TTS_RESPONSE = 352,
};

struct volc_tts {
    struct speech_socket socket;
    deepseek_turn_t *api;
    const char *speaker;
    char id[33];
    atomic_bool session_ready;
    size_t sent, pcm_bytes;
};

static int send_event(struct volc_tts *v, uint32_t event, const char *text);

static int connected(void *context)
{
    return send_event(context, START_CONNECTION, NULL);
}

static uint32_t number(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put_number(uint8_t *p, uint32_t value)
{
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
}

static int message(void *context, const uint8_t *data, size_t length)
{
    struct volc_tts *v = context;
    if (length < 8 || data[0] >> 4 != 1 || data[2] & 15)
        return -EPROTO;
    size_t header = (data[0] & 15) * 4;
    if (header < 4 || header + 4 > length)
        return -EPROTO;
    unsigned type = data[1] >> 4;
    if (type == 15) {
        ESP_LOGW("aster_ai", "volc service error=%lu", (unsigned long)number(data + header));
        return -EIO;
    }
    if ((type != 9 && type != 11) || (data[1] & 15) != 4)
        return -EPROTO;
    uint32_t event = number(data + header);
    if (event == CONNECTION_FAILED || event == SESSION_FAILED || event == SESSION_CANCELED) {
        ESP_LOGW("aster_ai", "volc service event=%lu", (unsigned long)event);
        return -EIO;
    }
    if (event == CONNECTION_STARTED)
        return send_event(v, START_SESSION, NULL);
    if (event == SESSION_STARTED) {
        atomic_store(&v->session_ready, true);
        ESP_LOGI("aster_ai", "volc session ready");
        return 0;
    }
    if (event == SESSION_FINISHED) {
        if (!v->pcm_bytes || v->pcm_bytes % 2)
            return -EPROTO;
        int rc = v->api->end_pcm(v->api->output);
        if (!rc)
            atomic_store(&v->socket.finished, true);
        return rc;
    }
    if (event != TTS_RESPONSE)
        return 0;
    if (type != 11 || header + 8 > length)
        return -EPROTO;
    size_t id_length = number(data + header + 4);
    size_t offset = header + 8;
    if (id_length != strlen(v->id) || id_length > length - offset ||
        memcmp(data + offset, v->id, id_length))
        return -EPROTO;
    offset += id_length;
    if (length - offset < 4)
        return -EPROTO;
    size_t pcm = number(data + offset);
    if (pcm != length - offset - 4 || pcm > 24000u * 2 * 120 - v->pcm_bytes)
        return -EPROTO;
    if (pcm && !v->pcm_bytes)
        ESP_LOGI("aster_ai", "volc first audio");
    v->pcm_bytes += pcm;
    return pcm ? v->api->receive_pcm(v->api->output, data + offset + 4, pcm) : 0;
}

static int send_event(struct volc_tts *v, uint32_t event, const char *text)
{
    char *json = NULL;
    if (event == START_SESSION || event == TASK_REQUEST) {
        cJSON *body = cJSON_CreateObject();
        cJSON *user = cJSON_AddObjectToObject(body, "user");
        cJSON *req = cJSON_AddObjectToObject(body, "req_params");
        cJSON *audio = cJSON_AddObjectToObject(req, "audio_params");
        bool ok = cJSON_AddStringToObject(user, "uid", "aster") &&
                  cJSON_AddNumberToObject(body, "event", event) &&
                  cJSON_AddStringToObject(body, "namespace", "BidirectionalTTS") &&
                  cJSON_AddStringToObject(req, "speaker", v->speaker) &&
                  cJSON_AddStringToObject(audio, "format", "pcm") &&
                  cJSON_AddNumberToObject(audio, "sample_rate", 24000) &&
                  (!text || cJSON_AddStringToObject(req, "text", text));
        if (ok)
            json = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        if (!json)
            return -ENOMEM;
    }
    const char *body = json ? json : "{}";
    size_t bytes = strlen(body);
    size_t id_length = event >= START_SESSION ? strlen(v->id) : 0;
    size_t cap = 12 + (id_length ? 4 + id_length : 0) + bytes;
    uint8_t *wire = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int rc = -ENOMEM;
    if (wire) {
        memcpy(wire, "\x11\x14\x10\x00", 4);
        put_number(wire + 4, event);
        size_t offset = 8;
        if (id_length) {
            put_number(wire + offset, id_length);
            memcpy(wire + offset + 4, v->id, id_length);
            offset += 4 + id_length;
        }
        put_number(wire + offset, bytes);
        memcpy(wire + offset + 4, body, bytes);
        rc = speech_socket_send(&v->socket, wire, cap, true);
        free(wire);
    }
    cJSON_free(json);
    return rc;
}

int volc_tts_open(struct volc_tts **out, deepseek_turn_t *api, const struct ai_speech_keys *keys)
{
    *out = NULL;
    struct volc_tts *v = heap_caps_calloc(1, sizeof(*v), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!v)
        return -ENOMEM;
    v->api = api;
    v->speaker = keys->speaker;
    v->socket.provider = "volc";
    snprintf(v->id, sizeof(v->id), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
    char headers[200];
    snprintf(headers, sizeof(headers),
             "X-Api-Key: %s\r\nX-Api-Resource-Id: seed-tts-2.0\r\nX-Api-Connect-Id: %s\r\n",
             keys->volc_key, v->id);
    int rc = speech_socket_open(&v->socket, "wss://openspeech.bytedance.com/api/v3/tts/bidirection",
                                headers, 2, 65536, message, connected, v);
    ai_secret_clear(headers, sizeof(headers));
    if (!rc)
        *out = v;
    else
        free(v);
    return rc;
}

static int send_text(struct volc_tts *v, const char *text, size_t length)
{
    if (length == v->sent)
        return 0;
    char *piece = strndup(text + v->sent, length - v->sent);
    if (!piece)
        return -ENOMEM;
    int rc = send_event(v, TASK_REQUEST, piece);
    free(piece);
    if (!rc) {
        if (!v->sent)
            ESP_LOGI("aster_ai", "volc first text sent");
        v->sent = length;
    }
    return rc;
}

int volc_tts_delta(struct volc_tts *v, const char *text, size_t length)
{
    int rc = atomic_load(&v->socket.error);
    if (!rc && atomic_load(&v->session_ready)) {
        /* Give synthesis enough context for natural cadence and keep the
         * portion after the punctuation for the next text update. */
        size_t clause = speech_first_clause(text + v->sent, length - v->sent);
        if (clause)
            rc = send_text(v, text, v->sent + clause);
    }
    return rc;
}

int volc_tts_finish(struct volc_tts *v, const char *text)
{
    bool (*cancelled)(void) = v->api->cancelled;
    int rc = speech_socket_wait(&v->socket, &v->session_ready, cancelled, 6500);
    if (!rc)
        rc = send_text(v, text, strlen(text));
    if (!rc)
        rc = send_event(v, FINISH_SESSION, NULL);
    if (!rc)
        rc = speech_socket_wait(&v->socket, &v->socket.finished, cancelled, 90000);
    if (!rc)
        (void)send_event(v, FINISH_CONNECTION, NULL);
    return rc;
}

void volc_tts_close(struct volc_tts *v)
{
    if (v) {
        speech_socket_close(&v->socket);
        free(v);
    }
}
