/* SPDX-License-Identifier: MIT */
#include "aster_setup_json.h"
#include <string.h>
#include <stdbool.h>
static void ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
        ++*p;
}
static int string(const char **p, char *out, size_t cap) {
    if (*(*p)++ != '"')
        return -1;
    size_t n = 0;
    while (**p && **p != '"') {
        unsigned char c = *(*p)++;
        if (c < 32)
            return -1;
        if (c == '\\') {
            c = *(*p)++;
            switch (c) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            default:
                return -1;
            }
        }
        if (n + 1 >= cap)
            return -1;
        out[n++] = (char)c;
    }
    if (**p != '"')
        return -1;
    ++*p;
    out[n] = 0;
    return 0;
}
const aster_setup_json_field *aster_setup_json_get(const aster_setup_json *j, const char *key) {
    for (size_t i = 0; i < j->count; i++)
        if (!strcmp(j->fields[i].key, key))
            return &j->fields[i];
    return NULL;
}
int aster_setup_json_parse(aster_setup_json *out, const char *p) {
    memset(out, 0, sizeof(*out));
    ws(&p);
    if (*p++ != '{')
        return -1;
    ws(&p);
    if (*p == '}') {
        p++;
        ws(&p);
        return *p ? -1 : 0;
    }
    for (;;) {
        if (out->count == 16)
            return -1;
        aster_setup_json_field *f = &out->fields[out->count];
        if (string(&p, f->key, sizeof(f->key)) || aster_setup_json_get(out, f->key))
            return -1;
        ws(&p);
        if (*p++ != ':')
            return -1;
        ws(&p);
        if (*p == '"') {
            if (string(&p, f->text, sizeof(f->text)))
                return -1;
        } else {
            if (*p < '0' || *p > '9')
                return -1;
            bool zero = *p == '0';
            const char *start = p;
            f->is_number = 1;
            do {
                unsigned d = *p++ - '0';
                if (f->number > (UINT64_MAX - d) / 10)
                    return -1;
                f->number = f->number * 10 + d;
            } while (*p >= '0' && *p <= '9');
            if (zero && p - start != 1)
                return -1;
        }
        out->count++;
        ws(&p);
        if (*p == '}') {
            p++;
            ws(&p);
            return *p ? -1 : 0;
        }
        if (*p++ != ',')
            return -1;
        ws(&p);
    }
}
int aster_setup_json_quote(char *out, size_t cap, const char *p) {
    size_t n = 0;
    if (cap < 3)
        return -1;
    out[n++] = '"';
    for (; *p; p++) {
        unsigned char c = *p;
        if (c < 32)
            return -1;
        if (c == '"' || c == '\\') {
            if (n + 1 >= cap)
                return -1;
            out[n++] = '\\';
        }
        if (n + 2 >= cap)
            return -1;
        out[n++] = (char)c;
    }
    out[n++] = '"';
    out[n] = 0;
    return (int)n;
}
