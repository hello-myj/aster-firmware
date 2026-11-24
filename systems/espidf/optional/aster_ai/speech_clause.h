/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Return a UTF-8 byte offset. Keep short comma-separated openings together,
 * and avoid treating decimal points or a period inside a word as a boundary. */
static inline size_t speech_first_clause(const char *text, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (length - i >= 3) {
            if (!memcmp(text+i, "。", 3) || !memcmp(text+i, "！", 3) || !memcmp(text+i, "？", 3))
                return i+3;
            if (i >= 24 && (!memcmp(text+i, "，", 3) || !memcmp(text+i, "；", 3) ||
                            !memcmp(text+i, "：", 3)))
                return i+3;
        }
        if (text[i] == '!' || text[i] == '?')
            return i+1;
        if (i >= 12 && text[i] == '.' && i+1 < length && text[i+1] == ' ')
            return i+1;
        if (i >= 32 && (text[i] == ',' || text[i] == ';' || text[i] == '\n'))
            return i+1;
    }
    return 0;
}

static inline bool speech_has_words(const char *s)
{
    while (*s) {
        size_t plain = strspn(s, " \n\t\r*_'\".!?,;:()[]");
        if (plain) {
            s += plain;
            continue;
        }
        static const char *const marks[] = {"。","，","！","？","；","：","”","’","」","』","）","…"};
        size_t i = 0;
        while (i < sizeof(marks) / sizeof(marks[0]) && strncmp(s, marks[i], 3))
            ++i;
        if (i == sizeof(marks) / sizeof(marks[0]))
            return true;
        s += 3;
    }
    return false;
}
