/* SPDX-License-Identifier: MIT
 * Internal transport service; errno values here are NOT the APP ABI.
 * Owners are session IDs, never APP pointers. Owner 1 is reserved for CLI.
 * All entry points return EAGAIN if the worker holds the service lock; callers
 * retry later rather than waiting for network progress. Owner cleanup must be
 * retried before retiring the session. The native bridge maps APP ABI errors.
 */
#ifndef ASTER_NET_SERVICE_H
#define ASTER_NET_SERVICE_H
#include <stddef.h>
#include <stdint.h>
#define ASTER_NET_CLI_OWNER UINT64_C(1)
#define ASTER_NET_SLOTS 2
#define ASTER_NET_HOST_MAX 253
#define ASTER_NET_TCP_BUFFER 2048
#define ASTER_NET_UDP_MAX 1200
#define ASTER_NET_UDP_DEPTH 2

enum aster_net_kind { ASTER_NET_TCP = 1, ASTER_NET_UDP = 2 };
enum aster_net_state {
    ASTER_NET_OPENING = 1, ASTER_NET_RESOLVING, ASTER_NET_CONNECTING,
    ASTER_NET_READY, ASTER_NET_EOF, ASTER_NET_FAILED, ASTER_NET_CANCELLED
};
enum aster_net_phase {
    ASTER_NET_SOCKET = 1, ASTER_NET_DNS, ASTER_NET_CONNECT,
    ASTER_NET_TX, ASTER_NET_RX, ASTER_NET_CANCEL
};
struct aster_net_snapshot {
    uint32_t handle, kind, state, phase;
    int32_t error; /* 0, or internal negative errno */
    uint32_t tx_queued, rx_queued, dropped;
    uint64_t tx_bytes, rx_bytes;
    uint16_t local_port;
};
struct aster_net_interface_info { uint32_t flags, ipv4; };
/* Read cached OS interface fields; does not create a worker or take a slot. */
int aster_net_interface_info(struct aster_net_interface_info *out);

struct aster_net_stats {
    uint32_t active, closing, psram_bytes, stack_bytes;
    uint32_t opened, closed, failed, cancelled, dropped;
};
/* Copies host/configuration before returning; timeout 1..30000 ms.
 * Returns a positive generation handle or negative errno. */
int aster_net_open(uint64_t owner, enum aster_net_kind kind,
                   const char *host, uint16_t port, uint32_t timeout_ms);
int aster_net_snapshot(uint64_t owner, uint32_t handle,
                       struct aster_net_snapshot *out);
/* TCP partial acceptance is allowed; UDP is atomic, including zero length.
 * UDP host+port are copied; queue depth is bounded. Send means queued locally.
 * TCP writes after observed EOF return EPIPE (native Zephyr has no half-close).
 * Pending TCP output fails after 5s without progress, UDP after 5s total. */
int aster_net_send(uint64_t owner, uint32_t handle, const void *data,
                   size_t length, const char *host, uint16_t port);
/* TCP cap > 0, EAGAIN until bytes/EOF/error. Buffered bytes precede error/EOF.
 * UDP returns EMSGSIZE + required length without consuming the packet.
 * UDP length 0 is a datagram, never EOF. IPv4 returned in network byte order. */
int aster_net_recv(uint64_t owner, uint32_t handle, void *data, size_t capacity,
                   size_t *required, uint32_t *ipv4, uint16_t *port);
/* Cancel retains status and buffered input, discards unsent output.
 * Close invalidates immediately; slot remains charged until worker cleanup.
 * Only the worker owns/closes fds; never free in-flight DNS user data. */
int aster_net_cancel(uint64_t owner, uint32_t handle);
int aster_net_close(uint64_t owner, uint32_t handle);
int aster_net_close_owner(uint64_t owner);
int aster_net_get_stats(struct aster_net_stats *out);
#endif
