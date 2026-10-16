/* SPDX-License-Identifier: MIT
 * Internal bounded HTTP service. Private errno domain, not the APP ABI.
 * Worker owns copied inputs; no borrowed APP pointers survive start().
 */
#ifndef ASTER_HTTP_SERVICE_H
#define ASTER_HTTP_SERVICE_H
#include "aster_http_codec.h"
#define ASTER_HTTP_CLI_OWNER UINT64_C(1)
enum aster_http_method { ASTER_HTTP_GET = 1, ASTER_HTTP_HEAD, ASTER_HTTP_POST };
enum aster_http_state {
    ASTER_HTTP_CONNECTING = 1,
    ASTER_HTTP_HANDSHAKE,
    ASTER_HTTP_SENDING,
    ASTER_HTTP_HEADERS,
    ASTER_HTTP_BODY,
    ASTER_HTTP_DONE,
    ASTER_HTTP_FAILED,
    ASTER_HTTP_CANCELLED
};
struct aster_http_options {
    enum aster_http_method method;
    const char *url, *headers;
    size_t headers_length;
    const void *body;
    size_t body_length;
    uint32_t timeout_ms; /* 1..600000, includes queue backpressure */
    uint64_t body_limit; /* 1..64 MiB, not an allocation size */
};
struct aster_http_snapshot {
    uint32_t handle, state, status, redirects, queued;
    int32_t error, tls_error, transport_error;
    uint32_t tls_verify;
    uint64_t received, content_length;
    bool has_length, headers_ready, tls;
};
struct aster_http_stats {
    uint32_t active, closing, tls_active, psram_bytes, stack_bytes, ca_bytes,
        cpu_yields;
};
int aster_http_svc_start(uint64_t owner, const struct aster_http_options *options);
int aster_http_svc_status(uint64_t owner, uint32_t handle,
                      struct aster_http_snapshot *out);
/* Positive bytes, EAGAIN while pending, buffered data before EOF/error. */
int aster_http_svc_read(uint64_t owner, uint32_t handle, void *out,
                    size_t capacity);
int aster_http_svc_header(uint64_t owner, uint32_t handle, const char *name,
                      unsigned index, char *out, size_t capacity,
                      size_t *length);
int aster_http_svc_cancel(uint64_t owner, uint32_t handle);
int aster_http_svc_close(uint64_t owner, uint32_t handle);
int aster_http_svc_close_owner(uint64_t owner);
/* Administrative CA replacement; PEM includes NUL, max 8192. NULL resets.
 * Denied while a TLS request (including a terminal unclosed one) owns quota. */
int aster_http_svc_ca_set(const char *pem, size_t length);
int aster_http_svc_get_stats(struct aster_http_stats *out);
/* Shared bounded protocol worker. Not an APP API. Allocates its stack once. */
int aster_http_svc_worker_wake(void);
void aster_http_svc_worker_signal(void);
/* Reserve the single verified TLS quota and obtain a worker-owned CA copy.
 * HTTP admission and administrative CA changes honor this reservation.
 * Release only after freeing the TLS context. Zero-length password/CA data
 * never enters logs. A lease remains charged through MQTT reconnect. */
int aster_http_svc_tls_acquire(char **pem, size_t *length);
int aster_http_svc_tls_release(char **pem);
#endif
