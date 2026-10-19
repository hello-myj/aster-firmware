/* SPDX-License-Identifier: MIT
 * Fixed-size ring of network state changes (N5.5). Coalescing rules:
 *  - a (owner, handle) keeps at most one entry; a new state replaces it, so a
 *    storm of flapping collapses to the latest state instead of queueing;
 *  - when the ring is full the OLDEST distinct handle is evicted: an APP that
 *    never reads cannot make the service allocate.
 * Producers are service workers (net/http/mqtt/ws ticks); the consumer is the
 * APP event loop. Both run on the APP/service threads, so a small spinlock is
 * enough and no blocking call is possible here.
 */
#include "aster_net_event.h"
#include "aster_http.h"
#include <errno.h>
#include <string.h>
#ifdef ASTER_NET_HOST_TEST
#include "net_host_platform.h" /* host mutex stubs, same convention as net.c */
#else
#include <zephyr/kernel.h>
#endif
struct entry {
    uint64_t owner;
    uint32_t handle, kind, state;
    bool used;
};
static struct entry ring[ASTER_NET_EVENT_SLOTS];
static uint32_t next_evict; /* round-robin victim selection */
K_MUTEX_DEFINE(ev_lock);
void aster_net_event_post(uint64_t owner, uint32_t handle, uint32_t kind, uint32_t state)
{
    if (!owner || !handle) return;
    k_mutex_lock(&ev_lock, K_FOREVER);
    struct entry *free_slot = NULL, *oldest = NULL;
    for (uint32_t i = 0; i < ASTER_NET_EVENT_SLOTS; ++i) {
        struct entry *e = &ring[i];
        if (e->used && e->owner == owner && e->handle == handle) {
            /* Same handle: replace in place. A repeat of the identical state
             * is not a transition, so drop it entirely. */
            if (e->state != state || e->kind != kind) {
                e->state = state;
                e->kind = kind;
            }
            k_mutex_unlock(&ev_lock);
            return;
        }
        if (!e->used && !free_slot) free_slot = e;
    }
    if (!free_slot) {
        oldest = &ring[next_evict % ASTER_NET_EVENT_SLOTS];
        next_evict++;
        free_slot = oldest;
    }
    free_slot->owner = owner;
    free_slot->handle = handle;
    free_slot->kind = kind;
    free_slot->state = state;
    free_slot->used = true;
    k_mutex_unlock(&ev_lock);
}
bool aster_net_event_take(uint64_t owner, uint32_t *handle, uint32_t *kind, uint32_t *state)
{
    if (!owner) return false;
    bool found = false;
    k_mutex_lock(&ev_lock, K_FOREVER);
    for (uint32_t i = 0; i < ASTER_NET_EVENT_SLOTS; ++i) {
        struct entry *e = &ring[i];
        if (!e->used || e->owner != owner) continue;
        if (handle) *handle = e->handle;
        if (kind) *kind = e->kind;
        if (state) *state = e->state;
        /* Release BEFORE the entry is reused so a producer cannot see a
         * half-cleared slot. */
        memset(e, 0, sizeof(*e));
        found = true;
        break;
    }
    k_mutex_unlock(&ev_lock);
    return found;
}
void aster_net_event_drop_owner(uint64_t owner)
{
    if (!owner) return;
    k_mutex_lock(&ev_lock, K_FOREVER);
    for (uint32_t i = 0; i < ASTER_NET_EVENT_SLOTS; ++i)
        if (ring[i].used && ring[i].owner == owner) memset(&ring[i], 0, sizeof(ring[i]));
    k_mutex_unlock(&ev_lock);
}
void aster_net_event_reset(void)
{
    k_mutex_lock(&ev_lock, K_FOREVER);
    memset(ring, 0, sizeof(ring));
    next_evict = 0;
    k_mutex_unlock(&ev_lock);
}
/* ---- stage timing (N5.5) ---- */
static struct aster_net_diag diag;
static uint32_t dns_n, connect_n, tls_n, ttfb_n;
K_MUTEX_DEFINE(diag_lock);
static void fold(uint32_t v, uint32_t *last, uint32_t *avg, uint32_t *max, uint32_t *n)
{
    if (!v) return;                 /* stage not measured */
    *last = v;
    if (v > *max) *max = v;
    /* Running mean without 64-bit division on a u32 count. */
    uint32_t count = *n + 1;
    *avg = (uint32_t)(((uint64_t)*avg * *n + v) / count);
    *n = count;
}
void aster_net_diag_sample(uint32_t dns_us, uint32_t connect_us, uint32_t tls_us)
{
    k_mutex_lock(&diag_lock, K_FOREVER);
    fold(dns_us, &diag.dns_last_us, &diag.dns_avg_us, &diag.dns_max_us, &dns_n);
    fold(connect_us, &diag.connect_last_us, &diag.connect_avg_us, &diag.connect_max_us, &connect_n);
    fold(tls_us, &diag.tls_last_us, &diag.tls_avg_us, &diag.tls_max_us, &tls_n);
    if (dns_us || connect_us || tls_us) diag.samples++;
    k_mutex_unlock(&diag_lock);
}
void aster_net_diag_sample_ttfb(uint32_t ttfb_us)
{
    k_mutex_lock(&diag_lock, K_FOREVER);
    fold(ttfb_us, &diag.ttfb_last_us, &diag.ttfb_avg_us, &diag.ttfb_max_us, &ttfb_n);
    k_mutex_unlock(&diag_lock);
}
int aster_net_diag_get(struct aster_net_diag *out)
{
    if (!out) return -EINVAL;
    k_mutex_lock(&diag_lock, K_FOREVER);
    *out = diag;
    k_mutex_unlock(&diag_lock);
    return 0;
}
