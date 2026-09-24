/* SPDX-License-Identifier: MIT */
#ifndef ASTER_KV_H
#define ASTER_KV_H

#include "types.h"

/* Per-app key/value store (small persistent config blobs). */
ASTER_I("aster_kv_set") int32_t aster_kv_set(const char *key, uint32_t key_len,
		       const void *val, uint32_t val_len);
/* Returns value length on success; writes min(len, buffer) bytes. */
ASTER_I("aster_kv_get") int32_t aster_kv_get(const char *key, uint32_t key_len,
		       void *dst, uint32_t dst_max);
ASTER_I("aster_kv_delete") int32_t aster_kv_delete(const char *key, uint32_t key_len);

#endif /* ASTER_KV_H */
