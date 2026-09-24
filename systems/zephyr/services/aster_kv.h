/* SPDX-License-Identifier: MIT
 * Per-app key-value store backed by files under /SD:/aster/system/kv.
 * File name: <id>.<key>.  Simple, durable enough for V1 config blobs.
 */
#ifndef ASTER_KV_H
#define ASTER_KV_H

#include <stddef.h>
#include <stdint.h>

int aster_kv_set(const char *id, const char *key, const void *val, size_t len);
int aster_kv_get(const char *id, const char *key, void *dst, size_t max, size_t *out_len);
int aster_kv_delete(const char *id, const char *key);

#endif
