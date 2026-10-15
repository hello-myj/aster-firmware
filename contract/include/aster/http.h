/* SPDX-License-Identifier: MIT
 * ABI 3.9: bounded asynchronous HTTP/1.1. Permission "net"; requires
 * "http", and "tls" for HTTPS. No Wi-Fi configuration permission implied.
 */
#ifndef ASTER_HTTP_H
#define ASTER_HTTP_H
#include "types.h"
#define ASTER_HTTP_METHOD_GET 1u
#define ASTER_HTTP_METHOD_HEAD 2u
#define ASTER_HTTP_METHOD_POST 3u
#define ASTER_HTTP_STATE_CONNECTING 1u
#define ASTER_HTTP_STATE_HANDSHAKE 2u
#define ASTER_HTTP_STATE_SENDING 3u
#define ASTER_HTTP_STATE_HEADERS 4u
#define ASTER_HTTP_STATE_BODY 5u
#define ASTER_HTTP_STATE_DONE 6u
#define ASTER_HTTP_STATE_FAILED 7u
#define ASTER_HTTP_STATE_CANCELLED 8u
#define ASTER_HTTP_FLAG_HEADERS 1u
#define ASTER_HTTP_FLAG_LENGTH 2u
#define ASTER_HTTP_FLAG_TLS 4u
#define ASTER_HTTP_CERT_EXPIRED 1u
#define ASTER_HTTP_CERT_REVOKED 2u
#define ASTER_HTTP_CERT_NAME 4u
#define ASTER_HTTP_CERT_UNTRUSTED 8u
#define ASTER_HTTP_CERT_FUTURE 16u
#define ASTER_HTTP_CERT_WEAK 32u
#define ASTER_HTTP_CERT_OTHER 64u
#define ASTER_HTTP_URL_LIMIT 1024u
#define ASTER_HTTP_HEADERS_LIMIT 2048u
#define ASTER_HTTP_UPLOAD_LIMIT 2048u
#define ASTER_HTTP_READ_LIMIT 2048u
#define ASTER_HTTP_BODY_LIMIT 67108864u
typedef struct {
    uint32_t size, version; /* returned sizeof(struct), 1; layout: 56 bytes */
    uint32_t state, status; /* HTTP status, including 4xx/5xx; 0 before headers */
    int32_t error; /* stable ASTER_E...; HTTP errors are not transport errors */
    uint32_t redirects, queued, flags, certificate_flags, reserved;
    uint64_t received, content_length; /* length valid only with FLAG_LENGTH */
} aster_http_status_t;
/* Positive handle or stable error. All inputs copied before returning;
 * byte lengths exclude NUL. URL is ASCII http(s), no userinfo/IPv6. Headers
 * are CRLF-separated fields, no terminating empty line, <=32 fields; Host,
 * Content-Length, Transfer-Encoding, Connection, Accept-Encoding, Expect and
 * Upgrade are service-owned. Optional empty inputs require NULL, length=0.
 * Only POST accepts a body. timeout 1..600000 ms includes DNS, TLS and reader
 * backpressure; body_limit 1..64 MiB limits traffic, not allocation size.
 * Two slots shared with TCP/UDP/CLI, one TLS slot. Terminal handles retain
 * quota until close/APP exit. Requests do not automatically reconnect/retry.
 */
ASTER_I("aster_http_request") int32_t aster_http_request(
    uint32_t method, const char *url, uint32_t url_length,
    const char *headers, uint32_t headers_length, const void *body,
    uint32_t body_length, uint32_t timeout_ms, uint32_t body_limit);
/* Nonblocking calls may return EAGAIN; retry on a later event/timer.
 * Output size >=sizeof(status), tail untouched, unaligned output allowed. */
ASTER_I("aster_http_status") int32_t aster_http_status(
    uint32_t handle, aster_http_status_t *status, uint32_t size);
/* capacity 1..2048. Buffered bytes precede error/cancel; 0 means successful
 * EOF only. Partial reads allowed. No data is consumed on invalid output. */
ASTER_I("aster_http_read") int32_t aster_http_read(
    uint32_t handle, void *data, uint32_t capacity);
/* Case-insensitive ASCII token name; index selects duplicate field. Values
 * are ASCII, no NUL added. Returns byte count, ENOENT, EAGAIN or EMSGSIZE.
 * required is mandatory (4 bytes), receives field length on success/EMSGSIZE;
 * it must not overlap output. capacity 0..2048; NULL only when capacity=0.
 * Headers are available after FLAG_HEADERS; trailers update as received. */
ASTER_I("aster_http_header") int32_t aster_http_header(
    uint32_t handle, const char *name, uint32_t name_length, uint32_t index,
    char *output, uint32_t capacity, uint32_t *required);
/* Cancel keeps buffered data/status; close immediately invalidates the
 * handle, cleanup may briefly retain quota. APP stop/trap/init failure closes
 * its session. No borrowed guest pointer survives any call.
 * Redirects <=3, cross-origin drops all custom headers; HTTPS downgrade is
 * denied. TLS validates chain, name and date; a synchronized clock is needed.
 * Root set is device/admin-owned (default ISRG Root X1), no skip-verify API.
 * No HTTP2, decompression, cookie jar, persistent connections or listeners.
 */
ASTER_I("aster_http_cancel") int32_t aster_http_cancel(uint32_t handle);
ASTER_I("aster_http_close") int32_t aster_http_close(uint32_t handle);
#endif
