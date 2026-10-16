/* SPDX-License-Identifier: MIT
 * ABI 3.11: bounded asynchronous MQTT 3.1.1. Permission "net", feature
 * "mqtt"; TLS connections additionally require "tls". No Wi-Fi setup API.
 */
#ifndef ASTER_MQTT_H
#define ASTER_MQTT_H
#include "types.h"
#define ASTER_MQTT_TOPIC_LIMIT 128u
#define ASTER_MQTT_PAYLOAD_LIMIT 1024u
#define ASTER_MQTT_FLAG_TLS 1u
#define ASTER_MQTT_FLAG_RECONNECT 2u
#define ASTER_MQTT_FLAG_WILL_RETAIN 4u
#define ASTER_MQTT_MESSAGE_RETAIN 1u
#define ASTER_MQTT_MESSAGE_DUP 2u
#define ASTER_MQTT_STATE_CONNECTING 1u
#define ASTER_MQTT_STATE_HANDSHAKE 2u
#define ASTER_MQTT_STATE_CONNACK 3u
#define ASTER_MQTT_STATE_READY 4u
#define ASTER_MQTT_STATE_BACKOFF 5u
#define ASTER_MQTT_STATE_FAILED 6u
#define ASTER_MQTT_STATE_CANCELLED 7u
typedef struct {
    uint32_t size, version; /* sizeof=80, version=1; reserved must be zero */
    uint32_t flags, port, timeout_ms, keepalive_s, will_qos, reserved;
    /* Exact byte lengths, excluding NUL; no UINT32_MAX string mode.
     * Absent optional fields MUST be {0,0}. Password/will_payload binary. */
    aster_string_t host, client_id, username, password, will_topic, will_payload;
} aster_mqtt_options_t;
typedef struct {
    uint32_t size, version, state;
    int32_t error;
    uint32_t reconnects, queued, pending, subscriptions, received, sent;
    uint32_t connack, last_operation, last_operation_kind;
    int32_t operation_error;
    uint32_t certificate_flags, reserved; /* certificate bits: ASTER_HTTP_CERT_* */
} aster_mqtt_status_t; /* sizeof=64 */
typedef struct {
    uint32_t size, version, length, topic_length, qos, flags;
    char topic[128]; /* exact topic_length bytes; no NUL required */
    uint8_t data[1024]; /* exact length bytes, may contain NUL or be empty */
} aster_mqtt_message_t; /* sizeof=1176; unused bytes zero */
/* Positive opaque handle or stable ASTER_E*. All inputs copied on success.
 * Host: ASCII DNS/IPv4 <=253B; client ID UTF-8 1..64B; username <=128B,
 * password <=128B and requires username; Will topic <=128B, payload <=256B.
 * port 1..65535; timeout 100..30000ms per connection attempt;
 * keepalive 5..300s, serviced without APP polling. QoS only 0/1.
 * clean_session=true. Two transport slots globally shared with TCP/UDP/HTTP,
 * one verified TLS slot. No caller CA/skip-verify; synchronized clock required.
 * Every call is nonblocking and may return EAGAIN; retry on a later timer.
 */
ASTER_I("aster_mqtt_connect") int32_t aster_mqtt_connect(
    const aster_mqtt_options_t *options, uint32_t size);
ASTER_I("aster_mqtt_status") int32_t aster_mqtt_status(
    uint32_t handle, aster_mqtt_status_t *status, uint32_t size);
/* Publish/sub/unsub return a positive operation sequence, not delivery proof.
 * At most 2 queued operations, 4 filters, 2 received messages per client.
 * Topic names reject wildcards; filters permit full-level + and terminal #.
 * publish data=NULL only with length=0; retain 0/1; UTF-8 topic <=128 bytes.
 */
ASTER_I("aster_mqtt_publish") int32_t aster_mqtt_publish(uint32_t handle,
    const char *topic, uint32_t topic_length, const void *data, uint32_t length,
    uint32_t qos, uint32_t retain);
ASTER_I("aster_mqtt_subscribe") int32_t aster_mqtt_subscribe(uint32_t handle,
    const char *filter, uint32_t length, uint32_t qos);
ASTER_I("aster_mqtt_unsubscribe") int32_t aster_mqtt_unsubscribe(
    uint32_t handle, const char *filter, uint32_t length);
/* operation: OK writes stable completion error (inspect it); EAGAIN pending;
 * ENOENT unknown/evicted (last 4 completions). QoS0 means queued to transport;
 * QoS1 means PUBACK, not application/business acknowledgement.
 * recv: OK copies one whole message including empty payload; EAGAIN empty.
 * Invalid output never consumes data. Unaligned output allowed; tail untouched.
 */
ASTER_I("aster_mqtt_operation") int32_t aster_mqtt_operation(
    uint32_t handle, uint32_t sequence, int32_t *error);
ASTER_I("aster_mqtt_recv") int32_t aster_mqtt_recv(
    uint32_t handle, aster_mqtt_message_t *message, uint32_t size);
/* Reconnect backoff 1..30s restores confirmed subscriptions only. Pending ops
 * fail on loss; no offline/retry publish queue. Duplicates carry DUP; consumers
 * must drain messages or backpressure may lead to keepalive timeout.
 * cancel aborts (may trigger Will); close invalidates immediately and attempts
 * bounded DISCONNECT (unavailable links may still trigger Will). Terminal
 * handles retain quota until close/APP exit. Stop/trap/init failure closes all.
 */
ASTER_I("aster_mqtt_cancel") int32_t aster_mqtt_cancel(uint32_t handle);
ASTER_I("aster_mqtt_close") int32_t aster_mqtt_close(uint32_t handle);
#endif
