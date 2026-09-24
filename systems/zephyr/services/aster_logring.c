/* SPDX-License-Identifier: MIT */
#include "aster_logring.h"
#include "aster_mem.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>

#define ASTER_LOG_SLOTS 128

struct entry {
	uint64_t ts;
	uint32_t level;
	char     app[20];
	char     msg[ASTER_LOG_MAX_MSG];
};

/* 128 x 192 B = 24 KB of history that only ever moves under CPU loads and
 * stores -- no DMA engine reads it, so it has no claim on internal DRAM (that
 * is scarce here: AOT text and the display bounce both need it). Allocated
 * once from the PSRAM shared heap at boot. A static EXT_RAM_BSS_ATTR array
 * would be the other option but is unsafe: that memory is unreachable while
 * the cache is disabled, and log writes are not a controlled call site. */
static struct entry *ring;
static uint32_t head;
static uint32_t count;
static struct k_spinlock lock;

int aster_logring_init(void)
{
	if (ring) {
		return 0;
	}
	ring = aster_psram_alloc(sizeof(*ring) * ASTER_LOG_SLOTS);
	return ring ? 0 : -ENOMEM;
}

void aster_logring_write(const char *app_id, uint32_t level, const char *msg, size_t len)
{
	if (!ring) {
		return; /* no history buffer: logging degrades, nothing breaks */
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct entry *e = &ring[head % ASTER_LOG_SLOTS];
	head++;
	if (count < ASTER_LOG_SLOTS) {
		count++;
	}
	e->ts = k_uptime_get(); /* milliseconds (shell prints s.ms) */
	e->level = level;
	strncpy(e->app, app_id ? app_id : "-", sizeof(e->app) - 1);
	e->app[sizeof(e->app) - 1] = 0;
	if (len >= sizeof(e->msg)) {
		len = sizeof(e->msg) - 1;
	}
	memcpy(e->msg, msg, len);
	e->msg[len] = 0;
	k_spin_unlock(&lock, key);
}

void aster_logring_dump(aster_log_cb cb, void *u, int limit)
{
	if (!ring) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t n = count;
	/* the most recent `limit` entries, printed oldest -> newest (tail style) */
	uint32_t skip = (limit > 0 && (uint32_t)limit < n) ? n - (uint32_t)limit : 0;
	struct entry snapshot;
	for (uint32_t j = skip; j < n; j++) {
		uint32_t idx = (head - n + j) % ASTER_LOG_SLOTS;
		snapshot = ring[idx];
		k_spin_unlock(&lock, key);
		if (!cb(snapshot.ts, snapshot.app, snapshot.level, snapshot.msg, u)) {
			return;
		}
		key = k_spin_lock(&lock);
	}
	k_spin_unlock(&lock, key);
}

void aster_logring_clear(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	head = 0;
	count = 0;
	k_spin_unlock(&lock, key);
}
