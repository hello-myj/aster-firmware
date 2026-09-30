/* SPDX-License-Identifier: MIT
 * ASTER ABI 3.6 - read-only wall-clock time.
 */
#ifndef ASTER_TIME_H
#define ASTER_TIME_H

#include "types.h"

#define ASTER_TIME_FLAG_SYNCED (1u << 0)

typedef struct {
	uint32_t size;       /* set to sizeof(aster_time_info_t) */
	uint32_t version;    /* 1 */
	uint64_t unix_ms;    /* UTC milliseconds since 1970-01-01 00:00:00 */
	uint32_t flags;      /* ASTER_TIME_FLAG_*; clear means default time */
	int32_t year;        /* local calendar, full year */
	uint32_t month;      /* local calendar, 1..12 */
	uint32_t day;        /* local calendar, 1..31 */
	uint32_t hour;       /* local calendar, 0..23 */
	uint32_t minute;     /* local calendar, 0..59 */
	uint32_t second;     /* local calendar, 0..60 */
	uint32_t weekday;    /* local calendar, Sunday = 0 */
} aster_time_info_t;

/* Read wall-clock time without requesting or triggering network activity.
 * unix_ms is always UTC; broken-down calendar fields use the board's local
 * timezone. Before the first successful SNTP sync, the device returns its
 * configured default clock and leaves ASTER_TIME_FLAG_SYNCED clear. */
ASTER_I("aster_time_get") int32_t aster_time_get(
	aster_time_info_t *out, uint32_t size);

#endif /* ASTER_TIME_H */
