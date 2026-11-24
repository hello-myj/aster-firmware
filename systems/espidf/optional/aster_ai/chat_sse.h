/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct chat_sse {
    char line[4096];
    size_t used;
    bool done;
    void *context;
    int (*json)(void *context, const char *data);
};

int chat_sse_feed(struct chat_sse *s, const uint8_t *bytes, size_t length);
int chat_sse_finish(struct chat_sse *s);
