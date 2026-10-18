/* SPDX-License-Identifier: MIT
 * Internal mDNS publisher contract (N5.2, design: docs/network-mdns-design.zh-CN.md).
 * Runs on the existing HTTP/MQTT/WS worker tick: no thread, no timer. Published
 * only while an owner (Wi-Fi settings UI or diagnostic CLI) requests it. Not
 * an APP ABI; errno values stay private.
 */
#ifndef ASTER_MDNS_SERVICE_H
#define ASTER_MDNS_SERVICE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ASTER_MDNS_NAME_MAX 63u /* wire bytes excluding trailing root label */
enum aster_mdns_owner_bit { ASTER_MDNS_OWNER_UI = 1u, ASTER_MDNS_OWNER_CLI = 2u };
enum aster_mdns_state { ASTER_MDNS_OFF = 0, ASTER_MDNS_WAIT_IP = 1,
    ASTER_MDNS_ANNOUNCING = 2, ASTER_MDNS_IDLE = 3, ASTER_MDNS_FAILED = 4 };
struct aster_mdns_snapshot {
    uint32_t state, want, conflicts, renames;
    int32_t error;
    char name[ASTER_MDNS_NAME_MAX + 1]; /* full "<host>.local" */
    char ipv4[16];
    uint32_t rx, tx, drops, announce;
};

/* Bitwise request/withdraw; either owner keeping a bit active holds the
 * publish. Wakes the worker when a transition was accepted; returns 0, or
 * -EINVAL on unknown bits. No socket work happens on the caller thread. */
int aster_mdns_svc_acquire(uint32_t owner_bit);
int aster_mdns_svc_release(uint32_t owner_bit);
/* -EAGAIN under lock contention; snapshot is a consistent copy otherwise. */
int aster_mdns_svc_status(struct aster_mdns_snapshot *out);
/* Called by the HTTP/MQTT/WS worker tick; true = busy (keeps polling alive). */
bool aster_mdns_svc_tick(void);
#endif
