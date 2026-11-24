/* SPDX-License-Identifier: MIT */
#include "chat_sse.h"
#include <errno.h>
#include <string.h>

static int line_end(struct chat_sse *s)
{
    if (s->used && s->line[s->used - 1] == '\r')
        --s->used;
    s->line[s->used] = 0;
    s->used = 0;
    if (strncmp(s->line, "data:", 5))
        return 0;
    char *data = s->line + 5;
    while (*data == ' ')
        ++data;
    if (!strcmp(data, "[DONE]")) {
        s->done = true;
        return 0;
    }
    return *data ? s->json(s->context, data) : 0;
}

int chat_sse_feed(struct chat_sse *s, const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length && !s->done; ++i) {
        if (bytes[i] == '\n') {
            int rc = line_end(s);
            if (rc)
                return rc;
        } else {
            if (!bytes[i] || s->used == sizeof(s->line) - 1)
                return -EPROTO;
            s->line[s->used++] = bytes[i];
        }
    }
    return 0;
}

int chat_sse_finish(struct chat_sse *s)
{
    int rc = s->used ? line_end(s) : 0;
    return rc ? rc : s->done ? 0 : -EPROTO;
}
