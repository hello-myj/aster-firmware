/* SPDX-License-Identifier: MIT. Included after the MQTT bridge helpers.
 * Public WebSocket ABI on top of the bounded internal service. Every guest
 * buffer is validated and copied on entry; no WASM pointer crosses into the
 * service or its worker, and no background thread calls the APP or LVGL.
 */
#if defined(CONFIG_ASTER_WEBSOCKET)
#include "aster_ws.h"
_Static_assert(sizeof(aster_ws_status_t) == 48, "WS status ABI");
_Static_assert(sizeof(aster_ws_message_t) == 4124, "WS message ABI");
_Static_assert(offsetof(aster_ws_message_t, data) == 28, "WS message data offset");
_Static_assert(ASTER_WS_STATE_CONNECTING == ASTER_WS_CONNECTING &&
    ASTER_WS_STATE_TLS == ASTER_WS_TLS && ASTER_WS_STATE_UPGRADE == ASTER_WS_UPGRADE &&
    ASTER_WS_STATE_READY == ASTER_WS_READY && ASTER_WS_STATE_CLOSING == ASTER_WS_CLOSING &&
    ASTER_WS_STATE_CLOSED == ASTER_WS_CLOSED && ASTER_WS_STATE_FAILED == ASTER_WS_FAILED &&
    ASTER_WS_STATE_CANCELLED == ASTER_WS_CANCELLED, "WS state ABI");
static int32_t ws_error(int rc, const struct aster_ws_snapshot *s)
{
    if (rc >= 0) return rc;
    /* Malformed or truncated protocol and bad UTF-8 are one stable domain;
     * a closed client whose buffer is drained reports EPIPE, not clock. */
    if (rc == -EPROTO || rc == -EILSEQ) return ASTER_EPROTO;
    if (rc == -ENODATA) return ASTER_EPIPE;
    if (rc == -ENOTSUP) return ASTER_ENOTSUP;
    if (rc == -EACCES)
        return s && http_certificate_flags(s->tls_verify) ? ASTER_ECERT : ASTER_EPERM;
    if (rc == -EIO && s && s->tls_error && !s->transport_error) return ASTER_ETLS;
    return network_error(rc);
}
static int32_t n_ws_connect(wasm_exec_env_t env, uint32_t off, uint32_t length,
    uint32_t timeout)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    char url[ASTER_WS_URL_LIMIT + 1];
    if (!http_string(env, off, length, ASTER_WS_URL_LIMIT, url, false))
        return ASTER_EINVAL;
    return ws_error(aster_ws_svc_open(owner, url, timeout), NULL);
}
static int32_t n_ws_status(wasm_exec_env_t env, uint32_t h, uint32_t off, uint32_t size)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    void *p = size >= sizeof(aster_ws_status_t)
        ? network_buf(env, off, sizeof(aster_ws_status_t)) : NULL;
    if (!p) return ASTER_EINVAL;
    struct aster_ws_snapshot s;
    rc = aster_ws_svc_status(owner, h, &s);
    if (rc) return ws_error(rc, NULL);
    aster_ws_status_t out = { .size=sizeof(out), .version=1, .state=s.state,
        .error=ws_error(s.error, &s), .queued=s.queued, .pending=s.pending,
        .received=s.received, .sent=s.sent, .close_code=s.close_code,
        .certificate_flags=http_certificate_flags(s.tls_verify) };
    memcpy(p, &out, sizeof(out)); return ASTER_OK;
}
static int32_t n_ws_send(wasm_exec_env_t env, uint32_t h, uint32_t opcode,
    uint32_t off, uint32_t length)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    if (opcode != ASTER_WS_OPCODE_TEXT && opcode != ASTER_WS_OPCODE_BINARY)
        return ASTER_EINVAL;
    if (length > ASTER_WS_MESSAGE_LIMIT) return ASTER_EMSGSIZE;
    if (!length && off) return ASTER_EINVAL;
    const void *data = length ? network_buf(env, off, length) : NULL;
    if (length && !data) return ASTER_EINVAL;
    return ws_error(aster_ws_svc_send(owner, h, opcode, data, length), NULL);
}
static int32_t n_ws_recv(wasm_exec_env_t env, uint32_t h, uint32_t off, uint32_t size)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    uint8_t *p = size >= sizeof(aster_ws_message_t)
        ? network_buf(env, off, sizeof(aster_ws_message_t)) : NULL;
    if (!p) return ASTER_EINVAL;
    /* The public message struct always carries the full 4096-byte buffer, so
     * recv never returns EMSGSIZE: a whole message fits by construction.
     * Data lands directly in guest memory; bytes beyond length stay intact.
     * Invalid output never consumes a message (validated before the service). */
    size_t n = 0; unsigned op = 0;
    rc = aster_ws_svc_recv(owner, h, p + offsetof(aster_ws_message_t, data),
        ASTER_WS_MESSAGE_LIMIT, &n, &op);
    if (rc) {
        if (rc == -EAGAIN) return ASTER_EAGAIN;
        struct aster_ws_snapshot s;
        int sr = aster_ws_svc_status(owner, h, &s);
        return ws_error(rc, sr ? NULL : &s);
    }
    uint32_t header[7] = { sizeof(aster_ws_message_t), 1, op, (uint32_t)n, 0, 0, 0 };
    memcpy(p, header, sizeof(header)); return ASTER_OK;
}
static int32_t n_ws_cancel(wasm_exec_env_t env, uint32_t h)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    return rc ? rc : ws_error(aster_ws_svc_cancel(owner, h), NULL);
}
static int32_t n_ws_close(wasm_exec_env_t env, uint32_t h)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    return rc ? rc : ws_error(aster_ws_svc_close(owner, h), NULL);
}
#else
#define WS_STUB(name,args) static int32_t name args { return ASTER_ENOTSUP; }
WS_STUB(n_ws_connect,(wasm_exec_env_t e,uint32_t o,uint32_t n,uint32_t t))
WS_STUB(n_ws_status,(wasm_exec_env_t e,uint32_t h,uint32_t o,uint32_t n))
WS_STUB(n_ws_send,(wasm_exec_env_t e,uint32_t h,uint32_t o,uint32_t d,uint32_t n))
WS_STUB(n_ws_recv,(wasm_exec_env_t e,uint32_t h,uint32_t o,uint32_t n))
WS_STUB(n_ws_cancel,(wasm_exec_env_t e,uint32_t h))
WS_STUB(n_ws_close,(wasm_exec_env_t e,uint32_t h))
#undef WS_STUB
#endif
