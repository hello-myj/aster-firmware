/* SPDX-License-Identifier: MIT
 * ABI 3.8: bounded asynchronous IPv4 TCP/UDP. Permission: "net";
 * requires: "network". Does not grant Wi-Fi configuration or auto-reconnect.
 */
#ifndef ASTER_NETWORK_H
#define ASTER_NETWORK_H
#include "types.h"
#define ASTER_NETWORK_TCP 1u
#define ASTER_NETWORK_UDP 2u
#define ASTER_NETWORK_HOST_MAX 253u
#define ASTER_NETWORK_IO_MAX 2048u
#define ASTER_NETWORK_UDP_MAX 1200u
#define ASTER_NETWORK_SLOTS 2u /* shared by all APPs and diagnostic CLI */
#define ASTER_NETWORK_OPENING 1u
#define ASTER_NETWORK_RESOLVING 2u
#define ASTER_NETWORK_CONNECTING 3u
#define ASTER_NETWORK_READY 4u
#define ASTER_NETWORK_EOF 5u
#define ASTER_NETWORK_FAILED 6u
#define ASTER_NETWORK_CANCELLED 7u
#define ASTER_NETWORK_PHASE_SOCKET 1u
#define ASTER_NETWORK_PHASE_DNS 2u
#define ASTER_NETWORK_PHASE_CONNECT 3u
#define ASTER_NETWORK_PHASE_TX 4u
#define ASTER_NETWORK_PHASE_RX 5u
#define ASTER_NETWORK_PHASE_CANCEL 6u

#define ASTER_NETWORK_IF_PRESENT 1u
#define ASTER_NETWORK_IF_UP 2u
#define ASTER_NETWORK_IF_CARRIER 4u /* raw driver flag, not proof of association */
#define ASTER_NETWORK_IF_IPV4 8u

typedef struct {
    uint32_t size, version; /* returned sizeof(struct), 1 */
    uint32_t flags;
    char ipv4[16]; /* local IPv4 hint, empty without a preferred address */
    uint32_t max_connections, tcp_buffer_bytes, udp_payload_max, udp_queue_depth;
} aster_network_info_t;
/* Read cached interface/address state and service limits; no scan/connect or
 * internet probe. A local address does not prove reachability. UP means the
 * interface is enabled; ESP carrier can remain set while unassociated.
 * Returns OK with zero flags when no interface exists. size >=sizeof(info).
 */
int32_t aster_network_get_info(
    aster_network_info_t *info, uint32_t size);

typedef struct {
    uint32_t size, version; /* returned sizeof(struct), 1 */
    uint32_t kind, state, phase;
    int32_t error; /* stable ASTER_E..., never platform errno */
    uint32_t tx_queued, rx_queued, dropped, local_port;
    uint64_t tx_bytes, rx_bytes;
} aster_network_status_t;
typedef struct {
    uint32_t size, version; /* returned sizeof(struct), 1 */
    uint32_t length; /* received length; required length on EMSGSIZE */
    uint32_t port; /* UDP source port on successful receive; TCP: 0 */
    char ipv4[16]; /* UDP dotted IPv4 on successful receive; TCP/error: empty */
} aster_network_packet_t;

/* All calls return promptly. EAGAIN means retry on a later event/timer.
 * Open returns a positive opaque handle, or ASTER_E.... Host strings use an
 * explicit length (no NUL, ASCII hostname/IPv4, 1..253); IPv6/multicast are
 * unsupported. TCP port 1..65535, timeout 1..30000 includes DNS+connect.
 * UDP open requires host=NULL, host_len=port=timeout_ms=0. No listener API.
 * Cancelled/failed/EOF handles retain their quota until close or APP exit.
 */
int32_t aster_network_open(
    uint32_t kind, const char *host, uint32_t host_len,
    uint32_t port, uint32_t timeout_ms);
/* length <=2048, UDP <=1200. TCP: host=NULL, host_len=port=0; returns
 * accepted byte count (may be partial). UDP: destination required; atomic
 * datagram enqueue including zero-length. data=NULL is allowed only at zero
 * length. Acceptance does not prove wire delivery. TCP output expires after
 * 5s without progress; UDP after 5s total including DNS/queue residence.
 * UDP TX/RX queue depth=2; oversized/full incoming packets drop and count.
 */
int32_t aster_network_send(
    uint32_t handle, const void *data, uint32_t length,
    const char *host, uint32_t host_len, uint32_t port);
/* capacity <=2048; zero allowed only for UDP (data may be NULL). packet is
 * mandatory, size >=sizeof(aster_network_packet_t); validates all outputs
 * before consuming data. No overlapping data/packet ranges. Returns byte
 * count, EAGAIN, or stable error. UDP EMSGSIZE preserves the packet and sets
 * required length. TCP 0 means EOF; UDP 0 means an empty datagram. Buffered
 * input precedes EOF/cancel/error. TCP writes after observed EOF fail EPIPE;
 * native Zephyr cannot continue writing after a peer half-close.
 */
int32_t aster_network_recv(
    uint32_t handle, void *data, uint32_t capacity,
    aster_network_packet_t *packet, uint32_t size);
/* size >=sizeof(status); larger buffers accepted, tail left untouched. */
int32_t aster_network_status(
    uint32_t handle, aster_network_status_t *status, uint32_t size);
/* Cancel retains buffered input/status, discards unsent data. Close makes
 * the handle invalid immediately; worker cleanup may briefly retain quota.
 * APP stop/trap/init failure automatically closes the entire session.
 */
int32_t aster_network_cancel(uint32_t handle);
int32_t aster_network_close(uint32_t handle);
#endif
