/* SPDX-License-Identifier: MIT
 * Host harness for the N5.5 event ring and stage diagnostics (ASan/UBSan).
 * Drives the real implementation; asserts coalescing, owner isolation, bounded
 * capacity with oldest-handle eviction, session cleanup and the running
 * last/avg/max folds.
 */
#define ASTER_NET_HOST_TEST
#include <errno.h>
#include <stdio.h>
#include "net_host_platform.h"
#include <errno.h>
#include <string.h>
/* The event module only needs the kernel mutex; the host platform header
 * already provides k_mutex_lock/unlock. Zephyr's kernel.h is stubbed for the
 * server build (that path is covered by the cross-compile, not this test). */
#include "../systems/zephyr/services/aster_net_event.c"

static void drain_owner(uint64_t owner, unsigned *count, uint32_t states[])
{
    uint32_t h, k, s;
    while (aster_net_event_take(owner, &h, &k, &s)) {
        if (states && *count < 16) states[*count] = s;
        ++*count;
    }
}

int main(void)
{
    uint32_t h = 0, k = 0, s = 0;
    uint32_t states[16] = { 0 };
    unsigned n = 0;

    /* 1) empty ring */
    assert(!aster_net_event_take(11, &h, &k, &s));

    /* 2) one transition, then the same state repeated: coalesced (not queued) */
    aster_net_event_post(11, 100, ASTER_NET_EVENT_TCP, 3u /* net CONNECTING */);
    aster_net_event_post(11, 100, ASTER_NET_EVENT_TCP, 3u /* net CONNECTING */);
    aster_net_event_post(11, 100, ASTER_NET_EVENT_TCP, 3u /* net CONNECTING */);
    n = 0; drain_owner(11, &n, states);
    assert(n == 1 && states[0] == 3u /* net CONNECTING */);

    /* 3) a newer state replaces the pending one: newest wins, no queue buildup */
    aster_net_event_post(11, 100, ASTER_NET_EVENT_TCP, 3u /* net CONNECTING */);
    aster_net_event_post(11, 100, ASTER_NET_EVENT_TCP, 4u /* net READY */);
    n = 0; drain_owner(11, &n, states);
    assert(n == 1 && states[0] == 4u /* net READY */);

    /* 4) owner isolation: an APP never observes another session's handle */
    aster_net_event_post(22, 200, ASTER_NET_EVENT_WS, 3u /* ws IDLE */);
    n = 0; drain_owner(11, &n, NULL);
    assert(n == 0);
    assert(aster_net_event_take(22, &h, &k, &s));
    assert(h == 200 && k == ASTER_NET_EVENT_WS && s == 3u /* ws IDLE */);
    assert(!aster_net_event_take(22, &h, &k, &s));

    /* 5) capacity: more distinct handles than slots never grows memory; the
     * oldest handle is evicted and the newest is always present. */
    for (uint32_t i = 0; i < ASTER_NET_EVENT_SLOTS + 4; ++i)
        aster_net_event_post(33, 300 + i, ASTER_NET_EVENT_HTTP, 5u /* http BODY */);
    unsigned total = 0;
    uint32_t seen_newest = 0;
    while (aster_net_event_take(33, &h, &k, &s)) {
        ++total;
        if (h == 300 + ASTER_NET_EVENT_SLOTS + 3) seen_newest = 1;
    }
    assert(total == ASTER_NET_EVENT_SLOTS);
    assert(seen_newest == 1);

    /* 6) cleanup drops only the retiring session */
    aster_net_event_post(44, 400, ASTER_NET_EVENT_MQTT, 4);
    aster_net_event_post(55, 500, ASTER_NET_EVENT_TCP, 4u /* net READY */);
    aster_net_event_drop_owner(44);
    n = 0; drain_owner(44, &n, NULL);
    assert(n == 0);
    assert(aster_net_event_take(55, &h, &k, &s) && h == 500);
    aster_net_event_reset();
    assert(!aster_net_event_take(55, &h, &k, &s));

    /* 7) invalid inputs are ignored, never stored */
    aster_net_event_post(0, 100, ASTER_NET_EVENT_TCP, 1);
    aster_net_event_post(66, 0, ASTER_NET_EVENT_TCP, 1);
    assert(!aster_net_event_take(66, &h, &k, &s));

    /* 8) stage diagnostics: last/avg/max, zero means "not measured" */
    struct aster_net_diag d;
    assert(aster_net_diag_get(&d) == 0 && d.samples == 0);
    aster_net_diag_sample(1000, 2000, 0);
    aster_net_diag_sample(3000, 0, 0);
    assert(aster_net_diag_get(&d) == 0);
    assert(d.dns_last_us == 3000 && d.dns_max_us == 3000 && d.dns_avg_us == 2000);
    assert(d.connect_last_us == 2000 && d.connect_max_us == 2000);
    assert(d.tls_max_us == 0); /* never measured */
    assert(d.samples == 2);
    aster_net_diag_sample_ttfb(500);
    assert(aster_net_diag_get(&d) == 0 && d.ttfb_last_us == 500 && d.ttfb_max_us == 500);
    assert(aster_net_diag_get(NULL) == -EINVAL);

    puts("N5.5 event coalescing, owner isolation, bounded ring and diag folds OK");
    return 0;
}