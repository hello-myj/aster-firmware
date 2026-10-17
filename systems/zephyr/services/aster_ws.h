/* SPDX-License-Identifier: MIT
 * Internal service contract. No public APP imports until CLI gates pass.
 */
#ifndef ASTER_WS_SERVICE_H
#define ASTER_WS_SERVICE_H
#include "aster_ws_codec.h"
#define ASTER_WS_CLI_OWNER UINT64_C(1)
enum aster_ws_state { ASTER_WS_CONNECTING=1, ASTER_WS_TLS, ASTER_WS_UPGRADE,
    ASTER_WS_READY, ASTER_WS_CLOSING, ASTER_WS_CLOSED, ASTER_WS_FAILED, ASTER_WS_CANCELLED };
struct aster_ws_snapshot {
    uint32_t state, queued, pending, received, sent, pings, pongs, close_code, tls_verify;
    int32_t error, tls_error, transport_error;
};
struct aster_ws_stats { uint32_t active, closing, tls_active, psram_bytes; };
int aster_ws_svc_open(uint64_t owner, const char *url, uint32_t timeout_ms);
int aster_ws_svc_send(uint64_t owner, uint32_t handle, unsigned opcode, const void *data, size_t length);
int aster_ws_svc_status(uint64_t owner, uint32_t handle, struct aster_ws_snapshot *out);
/* Full message, including empty text/binary; 0=message, EAGAIN=none.
 * EMSGSIZE reports required length/opcode without consuming the message. */
int aster_ws_svc_recv(uint64_t owner,uint32_t handle,void *data,size_t capacity,size_t *length,unsigned *opcode);
int aster_ws_svc_close(uint64_t owner,uint32_t handle);
int aster_ws_svc_cancel(uint64_t owner,uint32_t handle);
int aster_ws_svc_close_owner(uint64_t owner);
int aster_ws_svc_stats(struct aster_ws_stats *out);
bool aster_ws_svc_tick(void);
#endif
