/* SPDX-License-Identifier: MIT
 * ABI 3.12: bounded asynchronous RFC 6455 WebSocket/WSS client.
 * Permission "net"; feature "websocket"; wss:// additionally requires the
 * "tls" feature and the device trust store. No raw frame or listener API.
 */
#ifndef ASTER_WS_H
#define ASTER_WS_H
#include "types.h"
#define ASTER_WS_URL_LIMIT 1024u
#define ASTER_WS_MESSAGE_LIMIT 4096u
#define ASTER_WS_OPCODE_TEXT 1u
#define ASTER_WS_OPCODE_BINARY 2u
/* Values match the internal service state machine one-for-one; the bridge
 * static-asserts the equivalence so an internal reorder cannot silently
 * change the public ABI. CLOSING drains buffered messages; CLOSED/FAILED/
 * CANCELLED are terminal and retain quota until close or APP exit. */
#define ASTER_WS_STATE_CONNECTING 1u
#define ASTER_WS_STATE_TLS 2u
#define ASTER_WS_STATE_UPGRADE 3u
#define ASTER_WS_STATE_READY 4u
#define ASTER_WS_STATE_CLOSING 5u
#define ASTER_WS_STATE_CLOSED 6u
#define ASTER_WS_STATE_FAILED 7u
#define ASTER_WS_STATE_CANCELLED 8u
typedef struct {
    uint32_t size, version, state; /* returned sizeof(struct), 1, state */
    int32_t error; /* stable ASTER_E... once terminal; 0 before */
    uint32_t queued, pending; /* 1 full received message waiting; 1 APP send flushing */
    uint32_t received, sent; /* completed message counters */
    uint32_t close_code; /* peer close code once CLOSED; 0 before */
    uint32_t certificate_flags; /* ASTER_HTTP_CERT_* bits on WSS failure */
    uint32_t reserved[2]; /* must be 0 */
} aster_ws_status_t; /* sizeof=48 */
typedef struct {
    uint32_t size, version; /* returned sizeof(struct), 1; layout 4124 bytes */
    uint32_t opcode; /* ASTER_WS_OPCODE_* of the delivered message */
    uint32_t length; /* exact message bytes 0..4096; 0 is a valid empty message */
    uint32_t reserved[3]; /* must be 0 */
    uint8_t data[4096]; /* exact length bytes; untouched beyond length */
} aster_ws_message_t;
/* Every call is nonblocking and may return ASTER_EAGAIN; retry on a later
 * event/timer. connect copies the URL before returning: ASCII ws(s):// with
 * IPv4 or DNS host, optional port (80/443 by scheme), path may contain a
 * query, no userinfo/fragment/extensions/subprotocols; url_length 1..1024
 * bytes excluding NUL (exact bytes, no implicit terminator). timeout
 * 100..30000 ms bounds DNS, TCP, TLS and the HTTP upgrade together. Returns
 * a positive opaque handle or stable ASTER_E*.
 * One WebSocket client per device, shared with the diagnostic CLI; its TCP
 * transport uses the global two-slot budget shared with TCP/UDP/HTTP/MQTT,
 * and wss:// uses the single shared verified-TLS slot and the device/admin
 * trust store (no caller CA, no skip-verify, synchronized clock required).
 */
int32_t aster_ws_connect(
    const char *url, uint32_t url_length, uint32_t timeout_ms);
/* status/recv: output size must be >= sizeof(struct), tail untouched,
 * unaligned output allowed. EAGAIN on lock contention only. */
int32_t aster_ws_status(
    uint32_t handle, aster_ws_status_t *status, uint32_t size);
/* send: opcode TEXT/BINARY; text must be valid UTF-8. length 0..4096; data
 * NULL only with length 0. Accepted into one bounded outgoing slot: OK is
 * queue admission, never peer acknowledgement; EAGAIN while a previous
 * message is still flushing (safe to retry; never blind-resend on error). */
int32_t aster_ws_send(
    uint32_t handle, uint32_t opcode, const void *data, uint32_t length);
/* recv: delivers one whole message with boundary and opcode. Server
 * fragmentation (total <=4096) and interleaved control frames are handled
 * internally; the client masks all frames and answers pings automatically.
 * OK copies the message (length may be 0: a real empty message), EAGAIN
 * means none waiting, EPIPE means closed with the buffer drained. Failed or
 * cancelled handles return their stable terminal error instead, then keep
 * quota until close or APP exit. Invalid output never consumes a message. */
int32_t aster_ws_recv(
    uint32_t handle, aster_ws_message_t *message, uint32_t size);
/* cancel keeps the handle and a terminal state (CANCELLED) until close;
 * close immediately invalidates the handle, attempting a bounded close
 * handshake (grace <=1500 ms) before releasing transport/TLS quota. Idle
 * connections ping every 15 s and fail after 10 s without the matching
 * pong; an incomplete fragmented message fails after 30 s. No automatic
 * reconnect: after CLOSED/FAILED/CANCELLED, close then connect a new client.
 * APP stop, init failure and traps close the session and release resources;
 * handles belong to one APP session and are never accepted across sessions.
 */
int32_t aster_ws_cancel(uint32_t handle);
int32_t aster_ws_close(uint32_t handle);
#endif
