/* SPDX-License-Identifier: MIT */
#include "speech_socket.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static int open_transport(struct speech_socket *s, const char *uri, const char *headers)
{
    const char *scheme = "wss://";
    if (strncmp(uri, scheme, strlen(scheme)))
        return -EINVAL;
    const char *path = strchr(uri + strlen(scheme), '/');
    if (!path)
        return -EINVAL;
    s->tls_transport = esp_transport_ssl_init();
    if (!s->tls_transport)
        return -ENOMEM;
    esp_transport_ssl_crt_bundle_attach(s->tls_transport, esp_crt_bundle_attach);
    if (s->ciphersuites)
        esp_transport_ssl_set_ciphersuites_list(s->tls_transport, s->ciphersuites);
    s->ws_transport = esp_transport_ws_init(s->tls_transport);
    if (!s->ws_transport)
        return -ENOMEM;
    esp_transport_set_default_port(s->ws_transport, 443);
    const esp_transport_ws_config_t config = {
        .ws_path = path, .headers = headers, .propagate_control_frames = true,
    };
    return esp_transport_ws_set_config(s->ws_transport, &config) == ESP_OK ? 0 : -EIO;
}

static void event(void *context, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    struct speech_socket *s = context;
    const esp_websocket_event_data_t *e = data;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        atomic_store(&s->connected, true);
        if (s->on_connected) {
            int rc = s->on_connected(s->context);
            if (rc)
                atomic_store(&s->error, rc);
        }
        return;
    }
    if (atomic_load(&s->finished) || atomic_load(&s->error))
        return;
    if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR ||
        id == WEBSOCKET_EVENT_CLOSED) {
        int status = e ? e->error_handle.esp_ws_handshake_status_code : 0;
        int transport = 0, tls = 0, sock = 0;
        if (e && e->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            transport = e->error_handle.esp_tls_last_esp_err;
            tls = e->error_handle.esp_tls_stack_err;
            sock = e->error_handle.esp_transport_sock_errno;
        }
        int rc = status == 401 || status == 403 ? -EACCES :
                 status == 429 ? -EAGAIN : -ECONNRESET;
        atomic_store(&s->error, rc);
        ESP_LOGW("aster_ai", "%s socket closed event=%ld handshake=%d transport=%d tls=%d socket=%d",
                 s->provider, (long)id, status, transport, tls, sock);
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA || e->op_code >= 8)
        return;
    if (!e->payload_offset && e->op_code) {
        s->used = 0;
        s->opcode = e->op_code;
    }
    if (s->opcode != s->expected || e->data_len < 0 ||
        (size_t)e->data_len >= s->capacity - s->used) {
        atomic_store(&s->error, -EPROTO);
        return;
    }
    if (e->data_len)
        memcpy(s->frame + s->used, e->data_ptr, e->data_len);
    s->used += e->data_len;
    if (e->fin && e->payload_offset + e->data_len == e->payload_len) {
        s->frame[s->used] = 0;
        int rc = s->message(s->context, s->frame, s->used);
        if (rc)
            atomic_store(&s->error, rc);
        s->used = 0;
        s->opcode = 0;
        if (s->retire_on_final && s->ws_transport && atomic_load(&s->finished)) {
            /* Run on the socket's receive task so the descriptor cannot be
             * closed and recycled before shutdown. The owner still joins
             * the task and destroys TLS before starting the reply clients. */
            int fd = esp_transport_get_socket(s->ws_transport);
            if (fd >= 0)
                (void)shutdown(fd, SHUT_RDWR);
        }
    }
}

int speech_socket_open(struct speech_socket *s, const char *uri, const char *headers,
                       unsigned opcode, size_t capacity, int (*message)(void *, const uint8_t *, size_t),
                       int (*on_connected)(void *), void *context)
{
    /* ESP-IDF can log the complete upgrade request on a write error. */
    esp_log_level_set("transport_ws", ESP_LOG_NONE);
    esp_log_level_set("websocket_client", ESP_LOG_NONE);
    s->capacity = capacity;
    s->expected = opcode;
    s->message = message;
    s->on_connected = on_connected;
    s->context = context;
    s->frame = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->frame)
        return -ENOMEM;
    if (s->retire_on_final || s->ciphersuites) {
        int rc = open_transport(s, uri, headers);
        if (rc) {
            speech_socket_close(s);
            return rc;
        }
    }
    /* Incoming messages are reassembled in PSRAM above. The S31 only
     * needs enough internal transport space for an outgoing audio frame. */
#if CONFIG_IDF_TARGET_ESP32S31
    const int chunk_bytes = 2048;
#else
    const int chunk_bytes = 4096;
#endif
    const esp_websocket_client_config_t config = {
        .uri = uri, .headers = headers, .crt_bundle_attach = esp_crt_bundle_attach,
        .ext_transport = s->ws_transport,
        .disable_auto_reconnect = true, .buffer_size = chunk_bytes, .task_stack = 8192,
        .network_timeout_ms = 4500,
    };
    s->handle = esp_websocket_client_init(&config);
    if (s->handle && esp_websocket_register_events(s->handle, WEBSOCKET_EVENT_ANY, event, s) == ESP_OK &&
        esp_websocket_client_start(s->handle) == ESP_OK)
        return 0;
    speech_socket_close(s);
    return -EIO;
}

int speech_socket_send(struct speech_socket *s, const void *data, size_t bytes, bool binary)
{
    int rc = atomic_load(&s->error);
    if (rc)
        return rc;
    if (!atomic_load(&s->connected))
        return -EAGAIN;
    int result = binary ? esp_websocket_client_send_bin(s->handle, data, bytes, pdMS_TO_TICKS(800)) :
                          esp_websocket_client_send_text(s->handle, data, bytes, pdMS_TO_TICKS(800));
    if (result == (int)bytes)
        return 0;
    rc = atomic_load(&s->error);
    return rc ? rc : -ECONNRESET;
}

int speech_socket_wait(struct speech_socket *s, atomic_bool *flag, bool (*cancelled)(void), unsigned ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    while (!cancelled()) {
        int rc = atomic_load(&s->error);
        if (rc)
            return rc;
        if (atomic_load(flag))
            return 0;
        if (esp_timer_get_time() >= deadline)
            return -ETIMEDOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return -ECANCELED;
}

void speech_socket_close(struct speech_socket *s)
{
    if (s->handle) {
        esp_websocket_client_stop(s->handle);
        esp_websocket_client_destroy(s->handle);
        s->handle = NULL;
    }
    if (s->ws_transport) {
        esp_transport_destroy(s->ws_transport);
        s->ws_transport = NULL;
    }
    if (s->tls_transport) {
        esp_transport_destroy(s->tls_transport);
        s->tls_transport = NULL;
    }
    free(s->frame);
    s->frame = NULL;
}
