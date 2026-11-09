/* SPDX-License-Identifier: MIT */
#ifndef ASTER_SETUP_JSON_H
#define ASTER_SETUP_JSON_H
#include <stddef.h>
#include <stdint.h>
/* Strict flat JSON objects only. Duplicate keys, truncation, nested values,
 * embedded NUL and unsupported escapes are rejected, never silently accepted. */
typedef struct {
    char key[32], text[256];
    uint64_t number;
    int is_number;
} aster_setup_json_field;
typedef struct {
    aster_setup_json_field fields[16];
    size_t count;
} aster_setup_json;
int aster_setup_json_parse(aster_setup_json *out, const char *json);
const aster_setup_json_field *aster_setup_json_get(const aster_setup_json *j, const char *key);
int aster_setup_json_quote(char *out, size_t cap, const char *text);
#endif
