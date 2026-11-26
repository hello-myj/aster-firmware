/* SPDX-License-Identifier: MIT */
#include "deepseek.h"
#include "chat_sse.h"
#include "ai_credentials.h"
#include "avatar_tool.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define API "https://api.deepseek.com"
#define BOUNDARY "AsterVoice_713afbd2de54"
#define JSON_CAP 16384
#define HTTP_TIMEOUT_MS 12000
struct part { const void *data; size_t length; };
struct reply {
    char *json;
    struct chat_sse *sse;
    size_t length;
    bool correct_type;
};

struct exchange {
    deepseek_turn_t *turn;
    struct reply *reply;
    int error;
    int64_t deadline;
    bool response_started;
};

static esp_err_t response_event(esp_http_client_event_t *event)
{
    struct exchange *x = event->user_data;
    if (!x || x->error)
        return ESP_OK;
    struct reply *reply = x->reply;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        ++x->turn->connections;
        if (reply->sse)
            ESP_LOGI("aster_ai", "chat TLS ready");
    }
    if (reply->sse && event->event_id == HTTP_EVENT_HEADERS_SENT)
        ESP_LOGI("aster_ai", "chat headers sent");
    if (reply->sse && event->event_id == HTTP_EVENT_ON_HEADER && !x->response_started) {
        x->response_started = true;
        ESP_LOGI("aster_ai", "chat response headers");
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key, "Content-Type")) {
        const char *type = reply->json ? "application/json" : reply->sse ? "text/event-stream" : "audio/pcm";
        size_t n = strlen(type);
        const char *v = event->header_value;
        reply->correct_type = !strncasecmp(v, type, n) && (!v[n] || v[n] == ';' || v[n] == ' ');
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data_len)
        return ESP_OK;
    size_t n = (size_t)event->data_len;
    x->turn->http_status = esp_http_client_get_status_code(event->client);
    if (x->turn->cancelled() || esp_timer_get_time() > x->deadline)
        x->error = x->turn->cancelled() ? -ECANCELED : -ETIMEDOUT;
    else if (x->turn->http_status != 200 || !reply->correct_type)
        x->error = -EPROTO;
    else if (reply->json) {
        if (n >= JSON_CAP - reply->length)
            x->error = -EFBIG;
        else {
            memcpy(reply->json + reply->length, event->data, n);
            reply->json[reply->length + n] = 0;
        }
    } else if (reply->sse)
        x->error = chat_sse_feed(reply->sse, event->data, n);
    else if (n > 48000u * 120u - reply->length)
        x->error = -EFBIG;
    else
        x->error = x->turn->receive_pcm(x->turn->output, event->data, n);
    if (!x->error)
        reply->length += n;
    return ESP_OK;
}

void deepseek_close(deepseek_turn_t *turn)
{
    if (turn->connection) {
        esp_http_client_cleanup(turn->connection);
        turn->connection = NULL;
    }
}

static esp_http_client_handle_t client_for(deepseek_turn_t *turn, const char *url)
{
    esp_http_client_handle_t client = turn->connection;
    if (!client) {
        const esp_http_client_config_t config = {
            .url = url, .method = HTTP_METHOD_POST, .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true, .timeout_ms = HTTP_TIMEOUT_MS, .buffer_size = 4096,
            .buffer_size_tx = 4096, .keep_alive_enable = true, .event_handler = response_event,
        };
        client = esp_http_client_init(&config);
        if (!client)
            return NULL;
        turn->connection = client;
        char auth[AI_KEY_CAP + 8];
        snprintf(auth, sizeof(auth), "Bearer %s", turn->key);
        esp_err_t err = esp_http_client_set_header(client, "Authorization", auth);
        ai_secret_clear(auth, sizeof(auth));
        if (err != ESP_OK) {
            deepseek_close(turn);
            return NULL;
        }
    }
    return client;
}

int deepseek_warm_chat(deepseek_turn_t *turn)
{
    esp_http_client_handle_t client = client_for(turn, API "/models");
    if (!client)
        return -ENOMEM;
    esp_err_t rc = esp_http_client_set_method(client, HTTP_METHOD_GET);
    if (rc == ESP_OK)
        rc = esp_http_client_set_timeout_ms(client, 2500);
    if (rc == ESP_OK)
        rc = esp_http_client_perform(client);
    turn->http_status = esp_http_client_get_status_code(client);
    if (rc == ESP_OK)
        rc = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (rc == ESP_OK)
        rc = esp_http_client_set_timeout_ms(client, HTTP_TIMEOUT_MS);
    if (rc != ESP_OK || turn->http_status != 200) {
        deepseek_close(turn);
        return -EIO;
    }
    return 0;
}

static int post(deepseek_turn_t *turn, const char *url, const char *type,
                const struct part *parts, size_t count, struct reply *reply)
{
    if (turn->cancelled())
        return -ECANCELED;
    struct exchange x = {.turn = turn, .reply = reply, .deadline = esp_timer_get_time() + 90000000};
    turn->http_status = 0;
    esp_http_client_handle_t client = client_for(turn, url);
    if (!client)
        return -ENOMEM;
    size_t total = 0;
    for (size_t i = 0; i < count; ++i)
        total += parts[i].length;
    void *joined = NULL;
    const void *body = parts[0].data;
    if (count > 1) {
        joined = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!joined)
            return -ENOMEM;
        size_t offset = 0;
        for (size_t i = 0; i < count; ++i) {
            memcpy((char *)joined + offset, parts[i].data, parts[i].length);
            offset += parts[i].length;
        }
        body = joined;
    }
    int rc = -EIO;
    esp_err_t err = esp_http_client_set_url(client, url);
    if (err == ESP_OK)
        err = esp_http_client_set_user_data(client, &x);
    if (err == ESP_OK)
        err = esp_http_client_set_header(client, "Content-Type", type);
    if (err == ESP_OK)
        err = esp_http_client_set_post_field(client, body, (int)total);
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
        turn->http_status = esp_http_client_get_status_code(client);
        if (x.error)
            rc = x.error;
        else if (err == ESP_OK)
            rc = turn->http_status != 200 || !reply->correct_type ? -EPROTO :
                 reply->length ? 0 : -ENODATA;
    }
    if (err != ESP_OK)
        ESP_LOGW("aster_ai", "HTTPS request failed: %s", esp_err_to_name(err));
    esp_http_client_set_user_data(client, NULL);
    esp_http_client_set_post_field(client, NULL, 0);
    free(joined);
    if (rc)
        deepseek_close(turn);
    return turn->cancelled() ? -ECANCELED : rc;
}

static int json_response(deepseek_turn_t *turn, const char *url, const char *type,
                         const struct part *parts, size_t n, char *out, size_t cap)
{
    struct reply reply = {.json = heap_caps_malloc(JSON_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)};
    if (!reply.json)
        return -ENOMEM;
    int rc = post(turn, url, type, parts, n, &reply);
    if (!rc) {
        cJSON *root = cJSON_ParseWithLength(reply.json, reply.length + 1);
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "text"));
        if (!text || !*text)
            rc = -ENODATA;
        else if (strlen(text) >= cap)
            rc = -EFBIG;
        else
            memcpy(out, text, strlen(text) + 1);
        cJSON_Delete(root);
    }
    free(reply.json);
    return rc;
}

int deepseek_asr(deepseek_turn_t *turn, const int16_t *mono, size_t samples, char text[AI_QUESTION_CAP])
{
    static const char prefix[] = "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    static const char suffix[] = "\r\n--" BOUNDARY "--\r\n";
    uint8_t wav[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x80\x3e\0\0\0\x7d\0\0\x02\0\x10\0data";
    uint32_t bytes = samples * sizeof(*mono), riff = 36 + bytes;
    memcpy(wav + 4, &riff, 4);
    memcpy(wav + 40, &bytes, 4);
    const struct part parts[] = {{prefix, sizeof(prefix)-1}, {wav, sizeof(wav)}, {mono, bytes}, {suffix, sizeof(suffix)-1}};
    return json_response(turn, API "/v1/tools/asr", "multipart/form-data; boundary=" BOUNDARY,
                         parts, 4, text, AI_QUESTION_CAP);
}

static bool add_message(cJSON *messages, const char *role, const char *text)
{
    cJSON *m = cJSON_CreateObject();
    if (!m || !cJSON_AddItemToArray(messages, m)) {
        cJSON_Delete(m);
        return false;
    }
    return cJSON_AddStringToObject(m, "role", role) && cJSON_AddStringToObject(m, "content", text);
}

struct chat_reader {
    struct chat_sse sse;
    char *answer;
    size_t used;
    int (*on_delta)(void *, size_t);
    void *context;
    bool avatar_enabled;
    struct ai_avatar_call tool[];
};

static int chat_delta(void *ctx, const char *json)
{
    struct chat_reader *reader = ctx;
    cJSON *obj = cJSON_Parse(json);
    int rc = 0;
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(obj, "choices");
    if (!cJSON_IsArray(choices)) {
        rc = -EPROTO;
    } else {
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
        if (reader->avatar_enabled)
            ai_avatar_tool_collect(reader->tool, choice, delta);
        const char *content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(delta, "content"));
        if (content && *content) {
            size_t length = strlen(content);
            if (length >= AI_REPLY_CAP - reader->used)
                rc = -EFBIG;
            else {
                memcpy(reader->answer + reader->used, content, length + 1);
                reader->used += length;
                rc = reader->on_delta(reader->context, reader->used);
            }
        }
    }
    cJSON_Delete(obj);
    return rc;
}

int deepseek_chat(deepseek_turn_t *turn, const char *question, const struct deepseek_exchange *history,
                  size_t count, char answer[AI_REPLY_CAP], int (*on_delta)(void *, size_t),
                  void *context)
{
    bool avatar_enabled = turn->avatar && turn->avatar->count && turn->avatar_cue;
    cJSON *body = cJSON_CreateObject();
    cJSON *messages = cJSON_AddArrayToObject(body, "messages");
    cJSON *thinking = cJSON_AddObjectToObject(body, "thinking");
    bool ok = messages && thinking && cJSON_AddStringToObject(body, "model", DEEPSEEK_MODEL) &&
              cJSON_AddStringToObject(thinking, "type", "disabled") &&
              cJSON_AddBoolToObject(body, "stream", true) && cJSON_AddNumberToObject(body, "max_tokens", 256) &&
              add_message(messages, "system", "你是 Aster 语音助手小鲸。先用一句话直接回答；除非用户要求详细解释，一到两句话即可。用用户的语言，不用 Markdown。");
    if (ok && avatar_enabled)
        ok = ai_avatar_tool_add(body, turn->avatar) &&
             add_message(messages, "system", turn->avatar->explicit_only ?
               "仅在用户明确提出动作或表情要求时调用 avatar_perform。使用工具时将台词放进 speech。" :
               "当用户要求小鲸做动作或表情，或者动作能自然配合问候和情绪时，可以调用 avatar_perform。"
               "你可以自行选择与对话相符的动作或表情。角色会持续当前动作，普通回复可直接输出正文。"
               "使用工具时把全部台词放进 speech，不要先另发正文。用户要求恢复待机时再选择待机动作。");
    for (size_t i = 0; ok && i < count; ++i)
        ok = add_message(messages, "user", history[i].question) &&
             add_message(messages, "assistant", history[i].answer);
    ok = ok && add_message(messages, "user", question);
    char *wire = ok ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    if (!wire)
        return -ENOMEM;
    struct part part = {wire, strlen(wire)};
    size_t memory = sizeof(struct chat_reader) + (avatar_enabled ? sizeof(struct ai_avatar_call) : 0);
    struct chat_reader *reader = heap_caps_calloc(1, memory, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int rc = -ENOMEM;
    if (reader) {
        reader->answer = answer;
        reader->on_delta = on_delta;
        reader->context = context;
        reader->avatar_enabled = avatar_enabled;
        reader->sse.context = reader;
        reader->sse.json = chat_delta;
        struct reply reply = {.sse = &reader->sse};
        rc = post(turn, API "/chat/completions", "application/json", &part, 1, &reply);
        if (!rc)
            rc = chat_sse_finish(&reader->sse);
        if (!rc && avatar_enabled && reader->tool->seen) {
            bool accepted = ai_avatar_tool_resolve(reader->tool, turn->avatar, turn->avatar_cue,
                                                   reader->used ? NULL : answer, AI_REPLY_CAP);
            if (!accepted && !reader->used)
                strcpy(answer, "这个动作现在做不到。");
            if (accepted || !reader->used) {
                /* If the response already streamed text, keep that audio
                 * and publish only its new stage directions. */
                reader->used = strlen(answer);
                rc = reader->on_delta(reader->context, reader->used);
            }
            ESP_LOGI("aster_ai", "avatar tool accepted=%d", accepted);
        }
        if (!rc && !reader->used)
            rc = -ENODATA;
        free(reader);
    }
    cJSON_free(wire);
    return rc;
}

int deepseek_tts(deepseek_turn_t *turn, const char *answer)
{
    cJSON *body = cJSON_CreateObject();
    bool ok = cJSON_AddStringToObject(body, "text", answer) &&
              cJSON_AddStringToObject(body, "voice_id", "stella") &&
              cJSON_AddStringToObject(body, "format", "pcm");
    char *wire = ok ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    if (!wire)
        return -ENOMEM;
    struct part part = {wire, strlen(wire)};
    struct reply reply = {0};
    int rc = post(turn, API "/v1/tools/tts", "application/json", &part, 1, &reply);
    cJSON_free(wire);
    return !rc && reply.length % 2 ? -EPROTO : rc;
}
