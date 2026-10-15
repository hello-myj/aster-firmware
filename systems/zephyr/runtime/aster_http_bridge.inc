/* SPDX-License-Identifier: MIT. Included after the network bridge helpers. */
#if defined(CONFIG_ASTER_HTTP)
#include "aster_http.h"
_Static_assert(sizeof(aster_http_status_t) == 56, "HTTP ABI size");
_Static_assert(offsetof(aster_http_status_t, received) == 40, "HTTP ABI alignment");
_Static_assert(ASTER_HTTP_GET == ASTER_HTTP_METHOD_GET &&
               ASTER_HTTP_CANCELLED == ASTER_HTTP_STATE_CANCELLED, "HTTP ABI enums");
static uint32_t http_certificate_flags(uint32_t raw)
{
    /* Mbed TLS verification bits stay private; UINT32_MAX means not checked. */
    if (raw == UINT32_MAX) return 0;
    uint32_t out = raw & 15u;
    if (raw & 0x200u) out |= ASTER_HTTP_CERT_FUTURE;
    if (raw & 0x1c000u) out |= ASTER_HTTP_CERT_WEAK;
    if (raw & ~(15u | 0x200u | 0x1c000u)) out |= ASTER_HTTP_CERT_OTHER;
    return out;
}
static int32_t http_error(int rc, const struct aster_http_snapshot *s)
{
    if (rc >= 0) return rc;
    if (rc == -ENOENT) return ASTER_ENOENT;
    if (rc == -EPROTO) return ASTER_EPROTO;
    if (rc == -ELOOP) return ASTER_EREDIRECT;
    if (rc == -ENODATA) return ASTER_ECLOCK;
    if (rc == -EACCES)
        return s && http_certificate_flags(s->tls_verify) ? ASTER_ECERT : ASTER_EPERM;
    if (rc == -EIO && s && s->tls_error && !s->transport_error) return ASTER_ETLS;
    return network_error(rc);
}
static bool http_string(wasm_exec_env_t env, uint32_t off, uint32_t n,
                        uint32_t max, char *out, bool optional)
{
    if (!n) { out[0] = 0; return optional && !off; }
    if (n > max) return false;
    uint8_t *p = network_buf(env, off, n);
    if (!p || memchr(p, 0, n)) return false;
    memcpy(out, p, n); out[n] = 0;
    return true;
}
static int32_t n_http_request(wasm_exec_env_t env, uint32_t method,
    uint32_t u, uint32_t un, uint32_t h, uint32_t hn, uint32_t b,
    uint32_t bn, uint32_t timeout, uint32_t limit)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    char url[ASTER_HTTP_URL_LIMIT + 1], headers[ASTER_HTTP_HEADERS_LIMIT + 1];
    if (!http_string(env, u, un, ASTER_HTTP_URL_LIMIT, url, false) ||
        !http_string(env, h, hn, ASTER_HTTP_HEADERS_LIMIT, headers, true) ||
        bn > ASTER_HTTP_UPLOAD_LIMIT || (!bn && b)) return ASTER_EINVAL;
    void *body = bn ? network_buf(env, b, bn) : NULL;
    if (bn && !body) return ASTER_EINVAL;
    struct aster_http_options opts = { .method = method, .url = url,
        .headers = headers, .headers_length = hn, .body = body, .body_length = bn,
        .timeout_ms = timeout, .body_limit = limit };
    return http_error(aster_http_svc_start(owner, &opts), NULL);
}
static int32_t n_http_status(wasm_exec_env_t env, uint32_t handle,
                             uint32_t off, uint32_t size)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    uint8_t *p = size >= sizeof(aster_http_status_t)
        ? network_buf(env, off, sizeof(aster_http_status_t)) : NULL;
    if (!p) return ASTER_EINVAL;
    struct aster_http_snapshot snap;
    rc = aster_http_svc_status(owner, handle, &snap);
    if (rc) return http_error(rc, NULL);
    aster_http_status_t out = { .size = sizeof(out), .version = 1,
        .state = snap.state, .status = snap.status,
        .error = http_error(snap.error, &snap), .redirects = snap.redirects,
        .queued = snap.queued, .flags = (snap.headers_ready ? ASTER_HTTP_FLAG_HEADERS : 0) |
        (snap.has_length ? ASTER_HTTP_FLAG_LENGTH : 0) | (snap.tls ? ASTER_HTTP_FLAG_TLS : 0),
        .certificate_flags = http_certificate_flags(snap.tls_verify),
        .received = snap.received, .content_length = snap.content_length };
    memcpy(p, &out, sizeof(out)); return ASTER_OK;
}
static int32_t n_http_read(wasm_exec_env_t env, uint32_t handle,
                           uint32_t off, uint32_t cap)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    if (!cap || cap > ASTER_HTTP_READ_LIMIT) return ASTER_EINVAL;
    void *p = network_buf(env, off, cap);
    if (!p) return ASTER_EINVAL;
    /* Snapshot before read; negative terminal results need stable TLS mapping.
     * A transition between calls returns EAGAIN for a retry, never false EOF. */
    struct aster_http_snapshot snap;
    rc = aster_http_svc_status(owner, handle, &snap);
    if (rc) return http_error(rc, NULL);
    rc = aster_http_svc_read(owner, handle, p, cap);
    if (rc < 0 && rc != -EAGAIN && snap.state < ASTER_HTTP_DONE) return ASTER_EAGAIN;
    return http_error(rc, &snap);
}
static int32_t n_http_header(wasm_exec_env_t env, uint32_t handle,
    uint32_t name_off, uint32_t name_len, uint32_t index, uint32_t off,
    uint32_t cap, uint32_t required_off)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    char name[ASTER_HTTP_HEADERS_LIMIT + 1];
    if (!http_string(env, name_off, name_len, ASTER_HTTP_HEADERS_LIMIT, name, false) ||
        cap > ASTER_HTTP_READ_LIMIT || (!cap && off)) return ASTER_EINVAL;
    for (uint32_t i = 0; i < name_len; ++i)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || strchr("!#$%&'*+-.^_`|~", name[i])))
            return ASTER_EINVAL;
    uint8_t *required = network_buf(env, required_off, 4);
    char *out = cap ? (char *)network_buf(env, off, cap) : NULL;
    if (!required || (cap && !out) ||
        (cap && (uint64_t)off < (uint64_t)required_off + 4 &&
         (uint64_t)required_off < (uint64_t)off + cap)) return ASTER_EINVAL;
    size_t n = 0;
    rc = aster_http_svc_header(owner, handle, name, index, out, cap, &n);
    if (!rc || rc == -EMSGSIZE) { uint32_t value = n; memcpy(required, &value, 4); }
    return rc ? http_error(rc, NULL) : (int32_t)n;
}
static int32_t n_http_cancel(wasm_exec_env_t env, uint32_t h)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    return rc ? rc : http_error(aster_http_svc_cancel(owner, h), NULL);
}
static int32_t n_http_close(wasm_exec_env_t env, uint32_t h)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    return rc ? rc : http_error(aster_http_svc_close(owner, h), NULL);
}
#else
#define HTTP_STUB(name, args) static int32_t name args { return ASTER_ENOTSUP; }
HTTP_STUB(n_http_request, (wasm_exec_env_t e, uint32_t m, uint32_t u, uint32_t un,
    uint32_t h, uint32_t hn, uint32_t b, uint32_t bn, uint32_t t, uint32_t l))
HTTP_STUB(n_http_status, (wasm_exec_env_t e, uint32_t h, uint32_t o, uint32_t n))
HTTP_STUB(n_http_read, (wasm_exec_env_t e, uint32_t h, uint32_t o, uint32_t n))
HTTP_STUB(n_http_header, (wasm_exec_env_t e, uint32_t h, uint32_t s, uint32_t sn,
    uint32_t i, uint32_t o, uint32_t n, uint32_t r))
HTTP_STUB(n_http_cancel, (wasm_exec_env_t e, uint32_t h))
HTTP_STUB(n_http_close, (wasm_exec_env_t e, uint32_t h))
#undef HTTP_STUB
#endif
