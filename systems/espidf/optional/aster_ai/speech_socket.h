/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_websocket_client.h"

struct speech_socket {
    esp_websocket_client_handle_t handle;
    const char *provider;
    atomic_bool connected, finished;
    atomic_int error;
    uint8_t *frame;
    size_t used, capacity;
    unsigned opcode, expected;
    int (*message)(void *, const uint8_t *, size_t);
    int (*on_connected)(void *);
    void *context;
};

int speech_socket_open(struct speech_socket *s, const char *uri, const char *headers,
                       unsigned opcode, size_t capacity, int (*message)(void *, const uint8_t *, size_t),
                       int (*on_connected)(void *), void *context);
int speech_socket_send(struct speech_socket *s, const void *data, size_t bytes, bool binary);
int speech_socket_wait(struct speech_socket *s, atomic_bool *flag, bool (*cancelled)(void), unsigned ms);
void speech_socket_close(struct speech_socket *s);
