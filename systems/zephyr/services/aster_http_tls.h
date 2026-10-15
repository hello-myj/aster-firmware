/* SPDX-License-Identifier: MIT */
#ifndef ASTER_HTTP_TLS_H
#define ASTER_HTTP_TLS_H
#include <stddef.h>
#include <stdint.h>
struct aster_http_tls;
struct aster_http_tls_report {
    int error, transport_error;
    uint32_t verify;
};
/* All operations are worker-only, nonblocking when the crypto lock is busy.
 * CA must remain valid until free. Setup failure leaves no partial context. */
int aster_http_tls_new(struct aster_http_tls **out, uint64_t owner,
                       uint32_t handle, const char *host, const char *pem,
                       size_t pem_length, struct aster_http_tls_report *report);
int aster_http_tls_handshake(struct aster_http_tls *tls,
                             struct aster_http_tls_report *report);
int aster_http_tls_send(struct aster_http_tls *tls, const void *data,
                        size_t length, struct aster_http_tls_report *report);
int aster_http_tls_recv(struct aster_http_tls *tls, void *data, size_t capacity,
                        struct aster_http_tls_report *report);
int aster_http_tls_free(struct aster_http_tls **tls);
size_t aster_http_tls_context_bytes(void);
void aster_http_tls_heap(size_t *current, size_t *peak);
const char *aster_http_tls_default_ca(size_t *length);
#endif
