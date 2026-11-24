/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <string.h>

static inline size_t xfyun_strip_markers(char *text)
{
    char *read = text + strspn(text, " \t\r\n"), *out = text;
    while (*read) {
        if (!strncmp(read, "[E]", 3)) {
            read += 3;
            if (out == text || out[-1] == ' ')
                read += strspn(read, " ");
        } else {
            *out++ = *read++;
        }
    }
    while (out > text && strchr(" \t\r\n", out[-1]))
        --out;
    *out = 0;
    return out - text;
}
