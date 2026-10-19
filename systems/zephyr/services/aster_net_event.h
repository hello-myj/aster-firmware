/* SPDX-License-Identifier: MIT
 * Bounded network state-change notifications (N5.5). Internal service; the
 * APP sees them as ASTER_EVENT_SYS + ASTER_SYS_NET_STATE carrying only
 * handle/state/kind. No payload is copied and no pointer survives a call.
 */
#ifndef ASTER_NET_EVENT_H
#define ASTER_NET_EVENT_H
#include <stdbool.h>
#include <stdint.h>

enum aster_net_event_kind { ASTER_NET_EVENT_TCP = 1, ASTER_NET_EVENT_UDP = 2,
    ASTER_NET_EVENT_HTTP = 3, ASTER_NET_EVENT_MQTT = 4, ASTER_NET_EVENT_WS = 5 };
#define ASTER_NET_EVENT_SLOTS 8u
/* Record a transition. Only (owner, handle, state) changes are stored; a
 * repeat of the last state for the same handle is dropped (coalescing).
 * Overwrites the oldest entry when full, so memory is fixed. Safe from the
 * service worker; never blocks. */
void aster_net_event_post(uint64_t owner, uint32_t handle, uint32_t kind, uint32_t state);
/* Pop at most one pending event for this owner (round-robin fairness across
 * owners). Returns false when nothing is pending for @p owner. */
bool aster_net_event_take(uint64_t owner, uint32_t *handle, uint32_t *kind, uint32_t *state);
/* Drop every pending entry for one owner (APP stop/trap). */
void aster_net_event_drop_owner(uint64_t owner);
/* Drop every pending entry (owner 0 semantics: all). Used by tests/teardown. */
void aster_net_event_reset(void);

/* ---- N5.5 stage timing diagnostics (internal; `aster net diag`) ---- */
struct aster_net_diag {
    uint32_t samples;          /* finished connections folded in */
    /* Durations are milliseconds from k_uptime_get(). The _us suffix is
     * historical; `aster net diag` prints these as ms. */
    uint32_t dns_last_us, dns_avg_us, dns_max_us;
    uint32_t connect_last_us, connect_avg_us, connect_max_us;
    uint32_t tls_last_us, tls_avg_us, tls_max_us;
    uint32_t ttfb_last_us, ttfb_avg_us, ttfb_max_us;
};
/* Elapsed milliseconds, or 0 when the stage did not run or ran past 10 min. */
static inline uint32_t aster_net_diag_span_ms(int64_t started, int64_t now)
{
    if (started <= 0) return 0;
    int64_t d = now - started;
    return d > 0 && d < 600000 ? (uint32_t)d : 0;
}
/* Passing 0 for a stage means "not measured this time" and is ignored. */
void aster_net_diag_sample(uint32_t dns_us, uint32_t connect_us, uint32_t tls_us);
void aster_net_diag_sample_ttfb(uint32_t ttfb_us);
int aster_net_diag_get(struct aster_net_diag *out);
/* Number of live APP network sessions (owners other than the CLI), used by
 * `aster net stats` to show per-APP connection counts without leaking IDs. */
uint32_t aster_net_app_sessions(void);
#endif