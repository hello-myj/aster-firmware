/* SPDX-License-Identifier: MIT
 * ABI 4.0: one network import family. Permission "net". TCP/UDP require
 * feature "network", HTTP "http", MQTT "mqtt", WebSocket "websocket".
 * TLS still requires feature "tls". Wi-Fi configuration is not this ABI.
 *
 * Six imports replace the 3.x per-protocol imports. Layouts are fixed-width
 * and contain no host pointers. A short, overlapping, or unknown-version
 * buffer returns ASTER_EINVAL. An operation the protocol does not implement
 * returns ASTER_ENOTSUP. Handles are per-APP, generation-tagged, and at most
 * four live entries; a stale, foreign, or CLI handle returns ASTER_EBADF.
 */
#ifndef ASTER_NET_H
#define ASTER_NET_H
#include "types.h"

#define ASTER_NET_ABI_VERSION 1u
#define ASTER_NET_PROTO_TCP  1u
#define ASTER_NET_PROTO_UDP  2u
#define ASTER_NET_PROTO_HTTP 3u
#define ASTER_NET_PROTO_MQTT 4u
#define ASTER_NET_PROTO_WS   5u

#define ASTER_NET_OP_SEND        1u
#define ASTER_NET_OP_RECV        2u
#define ASTER_NET_OP_HEADER      3u
#define ASTER_NET_OP_SUBSCRIBE   4u
#define ASTER_NET_OP_UNSUBSCRIBE 5u
#define ASTER_NET_OP_OPERATION   6u

#define ASTER_NET_CLASS_CONNECTING 1u
#define ASTER_NET_CLASS_READY      2u
#define ASTER_NET_CLASS_CLOSING    3u
#define ASTER_NET_CLASS_TERMINAL   4u

/* Open blob: this header, then aster_net_http_open_t or aster_net_mqtt_open_t
 * when proto is HTTP or MQTT. TCP, UDP and WebSocket have no tail. size is
 * the exact byte count of header plus tail. flags carries MQTT TLS/reconnect/
 * will-retain bits (ASTER_MQTT_FLAG_*). target is the host, URL, or empty. */
typedef struct {
	uint32_t size;
	uint32_t version; /* ASTER_NET_ABI_VERSION */
	uint32_t proto;
	uint32_t flags;
	uint32_t timeout_ms;
	uint32_t port;
	aster_string_t target;
	uint32_t reserved0;
	uint32_t reserved1;
} aster_net_open_hdr_t;

typedef struct {
	uint32_t method;
	uint32_t body_limit;
	aster_string_t headers;
	aster_string_t body;
} aster_net_http_open_t;

typedef struct {
	uint32_t keepalive_s;
	uint32_t will_qos;
	aster_string_t client_id;
	aster_string_t username;
	aster_string_t password;
	aster_string_t will_topic;
	aster_string_t will_payload;
} aster_net_mqtt_open_t;

typedef struct {
	uint32_t size;
	uint32_t version;
	uint32_t flags;
	char ipv4[16];
	uint32_t max_connections;
	uint32_t tcp_buffer_bytes;
	uint32_t udp_payload_max;
	uint32_t udp_queue_depth;
} aster_net_info_t;

/* status: this header plus the protocol tail. size must equal header+tail.
 * certificate_flags uses ASTER_HTTP_CERT_*. tx_queued/rx_queued are the
 * protocol's send/receive queue counts (HTTP queued bytes use rx_queued). */
typedef struct {
	uint32_t size;
	uint32_t version;
	uint32_t proto;
	uint32_t state_class;
	uint32_t proto_state;
	int32_t error;
	uint32_t certificate_flags;
	uint32_t tx_queued;
	uint32_t rx_queued;
	uint32_t reserved;
} aster_net_status_hdr_t;

typedef struct {
	uint32_t kind;
	uint32_t phase;
	uint32_t dropped;
	uint32_t local_port;
	uint64_t tx_bytes;
	uint64_t rx_bytes;
} aster_net_tcp_status_t;

typedef struct {
	uint32_t http_status;
	uint32_t redirects;
	uint32_t flags;
	uint32_t reserved;
	uint64_t received;
	uint64_t content_length;
} aster_net_http_status_t;

typedef struct {
	uint32_t reconnects;
	uint32_t subscriptions;
	uint32_t received;
	uint32_t sent;
	uint32_t connack;
	uint32_t last_operation;
	uint32_t last_operation_kind;
	int32_t operation_error;
} aster_net_mqtt_status_t;

typedef struct {
	uint32_t received;
	uint32_t sent;
	uint32_t close_code;
	uint32_t reserved;
} aster_net_ws_status_t;

/* io is in/out. name is a host, topic, filter, or header name. data is the
 * payload or the output buffer. flags: QoS in bits 0-1, retain in bit 2,
 * dup in bit 3, WebSocket opcode in bits 8-15. aux is the header index or
 * the MQTT operation sequence. required receives a byte count or, for
 * OPERATION, the completion error. port/ipv4 are the UDP peer. */
typedef struct {
	uint32_t size;
	uint32_t version;
	uint32_t flags;
	uint32_t port;
	aster_string_t name;
	aster_string_t data;
	uint32_t required;
	uint32_t aux;
	char ipv4[16];
} aster_net_io_t;

#if defined(__wasm__)
ASTER_I("aster_net_info") int32_t aster_net_info(aster_net_info_t *out, uint32_t size);
ASTER_I("aster_net_open") int32_t aster_net_open(const aster_net_open_hdr_t *req, uint32_t size);
ASTER_I("aster_net_io") int32_t aster_net_io(uint32_t handle, uint32_t op, aster_net_io_t *io, uint32_t size);
ASTER_I("aster_net_status") int32_t aster_net_status(uint32_t handle, aster_net_status_hdr_t *out, uint32_t size);
ASTER_I("aster_net_cancel") int32_t aster_net_cancel(uint32_t handle);
ASTER_I("aster_net_close") int32_t aster_net_close(uint32_t handle);
#endif
#endif /* ASTER_NET_H */
