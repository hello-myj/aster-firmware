/* SPDX-License-Identifier: MIT
 * App log ring: captures aster_log() output per app for `aster log` command.
 */
#ifndef ASTER_LOGRING_H
#define ASTER_LOGRING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define ASTER_LOG_MAX_MSG 160

/* Allocates the history buffer in PSRAM. Returns -ENOMEM if it could not be
 * had, in which case writes are dropped and `aster log` stays empty. */
int  aster_logring_init(void);
void aster_logring_write(const char *app_id, uint32_t level, const char *msg, size_t len);
/* Iterate newest-first; cb returns false to stop. `limit` max records. */
typedef bool (*aster_log_cb)(uint64_t ts, const char *app, uint32_t level, const char *msg, void *u);
void aster_logring_dump(aster_log_cb cb, void *u, int limit);
void aster_logring_clear(void);

#endif
