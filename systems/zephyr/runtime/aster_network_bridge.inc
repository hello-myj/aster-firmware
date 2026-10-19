/* SPDX-License-Identifier: MIT
 * Included after native bridge memory/permission helpers. All callbacks run
 * on the APP worker. Background networking receives only numeric owner IDs
 * and copied host/payload bytes; never ctx, env or guest pointers.
 */
#if IS_ENABLED(CONFIG_ASTER_NET)
#include "aster_net.h"
#include "aster_net_event.h"
#if defined(CONFIG_ASTER_MQTT)
#include "aster_mqtt.h"
#endif
#if defined(CONFIG_ASTER_WEBSOCKET)
#include "aster_ws.h"
#endif
#if defined(CONFIG_ASTER_HTTP)
#include "aster_http.h"
#endif
#include <errno.h>
#ifndef ASTER_NET_HOST_TEST
#include <zephyr/net/socket.h>
#endif
/* State numbers are part of the public ABI. Reject an accidental internal
 * reorder at compile time instead of silently returning different states. */
_Static_assert(ASTER_NET_TCP == ASTER_NETWORK_TCP && ASTER_NET_UDP == ASTER_NETWORK_UDP &&
 ASTER_NET_OPENING == ASTER_NETWORK_OPENING && ASTER_NET_RESOLVING == ASTER_NETWORK_RESOLVING &&
 ASTER_NET_CONNECTING == ASTER_NETWORK_CONNECTING && ASTER_NET_READY == ASTER_NETWORK_READY &&
 ASTER_NET_EOF == ASTER_NETWORK_EOF && ASTER_NET_FAILED == ASTER_NETWORK_FAILED &&
 ASTER_NET_CANCELLED == ASTER_NETWORK_CANCELLED, "network state ABI");
static uint64_t network_session = ASTER_NET_CLI_OWNER;
static int32_t network_error(int rc)
{
    if (rc >= 0) return rc;
    switch (-rc) {
    case EINVAL: return ASTER_EINVAL;
    case ENOMEM: return ASTER_ENOMEM;
    case EAGAIN: return ASTER_EAGAIN;
    case EBUSY: return ASTER_EBUSY;
    case EBADF: return ASTER_EBADF;
    case ENOSPC: return ASTER_ENOSPC;
    case ETIMEDOUT: return ASTER_ETIMEDOUT;
    case ECANCELED: return ASTER_ECANCELLED;
    case EMSGSIZE: return ASTER_EMSGSIZE;
    case EDESTADDRREQ: return ASTER_EDNS;
    case EPIPE: return ASTER_EPIPE;
    case ECONNRESET: case ENOTCONN: return ASTER_ECONNRESET;
    case ECONNREFUSED: return ASTER_ECONNREFUSED;
    case EHOSTUNREACH: case ENETUNREACH: case ENETDOWN: return ASTER_ENETUNREACH;
    case EAFNOSUPPORT: case ENOTSUP: return ASTER_ENOTSUP;
    default: return ASTER_EIO;
    }
}
static int32_t network_gate(wasm_exec_env_t env, uint64_t *owner)
{
    aster_app_ctx_t *ctx = ctx_of(env);
    if (!aster_ctx_perm(ctx, ASTER_PERM_NET)) return ASTER_EPERM;
    if (!ctx->network_owner) {
        if (network_session == INT64_MAX) return ASTER_ENOSPC;
        ctx->network_owner = ++network_session;
    }
    *owner = ctx->network_owner;
    return ASTER_OK;
}
/* Bounds without setting a WAMR exception: malformed ABI inputs return
 * EINVAL and leave the guest alive. No arithmetic in the 32-bit domain. */
static uint8_t *network_buf(wasm_exec_env_t env, uint32_t off, uint32_t len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    uint64_t end = 0;
    if (!inst || !off || !len ||
        !wasm_runtime_get_app_addr_range(inst, off, NULL, &end) ||
        (uint64_t)off + len > end) return NULL;
    return wasm_runtime_addr_app_to_native(inst, off);
}
/* Exact, bounded host strings: no implicit terminator or embedded NUL. */
static bool network_host(wasm_exec_env_t env, uint32_t off, uint32_t len, char *host)
{
    if (!len || len > ASTER_NETWORK_HOST_MAX) return false;
    uint8_t *p = network_buf(env, off, len);
    if (!p) return false;
    for (uint32_t i = 0; i < len; ++i) {
        uint8_t c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }
    memcpy(host, p, len); host[len] = 0;
    return true;
}
static int32_t n_network_get_info(wasm_exec_env_t env, uint32_t off, uint32_t size)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    (void)owner;
    if (size < sizeof(aster_network_info_t)) return ASTER_EINVAL;
    uint8_t *p = network_buf(env, off, sizeof(aster_network_info_t));
    if (!p) return ASTER_EINVAL;
    struct aster_net_interface_info fields;
    rc = aster_net_interface_info(&fields);
    if (rc) return network_error(rc);
    aster_network_info_t out = { .size = sizeof(out), .version = 1,
        .flags = fields.flags, .max_connections = ASTER_NET_SLOTS,
        .tcp_buffer_bytes = ASTER_NET_TCP_BUFFER,
        .udp_payload_max = ASTER_NET_UDP_MAX, .udp_queue_depth = ASTER_NET_UDP_DEPTH };
    if (fields.flags & ASTER_NETWORK_IF_IPV4)
        zsock_inet_ntop(AF_INET, &fields.ipv4, out.ipv4, sizeof(out.ipv4));
    memcpy(p, &out, sizeof(out)); return ASTER_OK;
}
static int32_t n_network_open(wasm_exec_env_t env, uint32_t kind,
    uint32_t off, uint32_t len, uint32_t port, uint32_t timeout)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    char host[ASTER_NETWORK_HOST_MAX + 1];
    if (kind == ASTER_NETWORK_UDP) {
        if (off || len || port || timeout) return ASTER_EINVAL;
        return network_error(aster_net_open(owner, ASTER_NET_UDP, NULL, 0, 5000));
    }
    if (kind != ASTER_NETWORK_TCP || !port || port > 65535 ||
        !timeout || timeout > 30000 || !network_host(env, off, len, host)) return ASTER_EINVAL;
    return network_error(aster_net_open(owner, ASTER_NET_TCP, host, port, timeout));
}
static int32_t n_network_send(wasm_exec_env_t env, uint32_t h, uint32_t off,
    uint32_t len, uint32_t host_off, uint32_t host_len, uint32_t port)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    if (len > ASTER_NETWORK_IO_MAX) return ASTER_EMSGSIZE;
    uint8_t *data = len ? network_buf(env, off, len) : NULL;
    if (len && !data) return ASTER_EINVAL;
    struct aster_net_snapshot s;
    rc = aster_net_snapshot(owner, h, &s);
    if (rc) return network_error(rc);
    char host[ASTER_NETWORK_HOST_MAX + 1];
    if (s.kind == ASTER_NET_UDP) {
        if (len > ASTER_NETWORK_UDP_MAX) return ASTER_EMSGSIZE;
        if (!port || port > 65535 || !network_host(env, host_off, host_len, host)) return ASTER_EINVAL;
    } else if (host_off || host_len || port) return ASTER_EINVAL;
    return network_error(aster_net_send(owner, h, data, len,
                         s.kind == ASTER_NET_UDP ? host : NULL, port));
}
static int32_t n_network_recv(wasm_exec_env_t env, uint32_t h, uint32_t off,
    uint32_t cap, uint32_t packet_off, uint32_t size)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    if (size < sizeof(aster_network_packet_t) || cap > ASTER_NETWORK_IO_MAX) return ASTER_EINVAL;
    uint8_t *packet = network_buf(env, packet_off, sizeof(aster_network_packet_t));
    uint8_t *data = cap ? network_buf(env, off, cap) : NULL;
    if (!packet || (cap && !data) ||
        (cap && (uint64_t)off < (uint64_t)packet_off + sizeof(aster_network_packet_t) &&
         (uint64_t)packet_off < (uint64_t)off + cap)) return ASTER_EINVAL;
    size_t required = 0; uint32_t ip = 0; uint16_t port = 0;
    rc = aster_net_recv(owner, h, data, cap, &required, &ip, &port);
    aster_network_packet_t out = { .size = sizeof(out), .version = 1,
                                   .length = required, .port = port };
    if (rc >= 0 && port) zsock_inet_ntop(AF_INET, &ip, out.ipv4, sizeof(out.ipv4));
    memcpy(packet, &out, sizeof(out));
    return network_error(rc);
}
static int32_t n_network_status(wasm_exec_env_t env, uint32_t h, uint32_t off, uint32_t size)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    if (rc) return rc;
    if (size < sizeof(aster_network_status_t)) return ASTER_EINVAL;
    uint8_t *p = network_buf(env, off, sizeof(aster_network_status_t));
    if (!p) return ASTER_EINVAL;
    struct aster_net_snapshot s;
    rc = aster_net_snapshot(owner, h, &s);
    if (rc) return network_error(rc);
    aster_network_status_t out = { .size = sizeof(out), .version = 1,
        .kind = s.kind, .state = s.state, .phase = s.phase,
        .error = network_error(s.error), .tx_queued = s.tx_queued,
        .rx_queued = s.rx_queued, .dropped = s.dropped, .local_port = s.local_port,
        .tx_bytes = s.tx_bytes, .rx_bytes = s.rx_bytes };
    memcpy(p, &out, sizeof(out)); return ASTER_OK;
}
static int32_t n_network_cancel(wasm_exec_env_t env, uint32_t h)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    return rc ? rc : network_error(aster_net_cancel(owner, h));
}
static int32_t n_network_close(wasm_exec_env_t env, uint32_t h)
{
    uint64_t owner; int32_t rc = network_gate(env, &owner);
    return rc ? rc : network_error(aster_net_close(owner, h));
}
#define NET_PUB_SLOTS 16
struct net_pub { uint64_t owner; uint32_t handle, proto, internal; int used; };
static struct net_pub net_pubs[NET_PUB_SLOTS];
static uint32_t net_pub_gen = 1;
void aster_net_pub_drop(uint64_t owner)
{
    for (int i = 0; i < NET_PUB_SLOTS; ++i)
        if (net_pubs[i].used && net_pubs[i].owner == owner) net_pubs[i].used = 0;
}
static int net_pub_count(uint64_t owner)
{
    int n = 0;
    for (int i = 0; i < NET_PUB_SLOTS; ++i)
        if (net_pubs[i].used && net_pubs[i].owner == owner) ++n;
    return n;
}
static int32_t net_pub_add(uint64_t owner, uint32_t proto, int32_t internal)
{
    if (internal <= 0) return internal;
    if (net_pub_count(owner) >= 4) return ASTER_ENOSPC;
    for (int i = 0; i < NET_PUB_SLOTS; ++i) if (!net_pubs[i].used) {
        uint32_t h = (net_pub_gen << 8) | (uint32_t)(i + 1);
        if (++net_pub_gen > 0x00fffffeu) net_pub_gen = 1;
        net_pubs[i].owner = owner; net_pubs[i].handle = h; net_pubs[i].proto = proto;
        net_pubs[i].internal = (uint32_t)internal; net_pubs[i].used = 1;
        return (int32_t)h;
    }
    return ASTER_ENOSPC;
}
static int net_pub_get(uint64_t owner, uint32_t handle, uint32_t *proto, uint32_t *internal)
{
    for (int i = 0; i < NET_PUB_SLOTS; ++i) if (net_pubs[i].used && net_pubs[i].handle == handle) {
        if (net_pubs[i].owner != owner) return 0;
        *proto = net_pubs[i].proto; *internal = net_pubs[i].internal; return 1;
    }
    return 0;
}
static void net_pub_del(uint32_t handle)
{
    for (int i = 0; i < NET_PUB_SLOTS; ++i)
        if (net_pubs[i].used && net_pubs[i].handle == handle) net_pubs[i].used = 0;
}
uint32_t aster_net_public_handle(uint64_t owner, uint32_t kind, uint32_t internal)
{
    for (int i = 0; i < NET_PUB_SLOTS; ++i)
        if (net_pubs[i].used && net_pubs[i].owner == owner && net_pubs[i].proto == kind &&
            net_pubs[i].internal == internal) return net_pubs[i].handle;
    return 0;
}
void aster_native_release_network(aster_app_ctx_t *ctx)
{
    if (!ctx || !ctx->network_owner) return;
    (void)net_pub_add; (void)net_pub_get; (void)net_pub_del;
    aster_net_pub_drop(ctx->network_owner);
    /* Wait only for the bounded service critical section, never DNS/socket
     * progress. Accept owner-close before retiring the session. Numeric IDs
     * remain unique while old DNS callbacks finish worker-owned cleanup. */
#if defined(CONFIG_ASTER_WEBSOCKET)
    while (aster_ws_svc_close_owner(ctx->network_owner) == -EAGAIN) k_sleep(K_MSEC(1));
#endif
#if defined(CONFIG_ASTER_MQTT)
    while (aster_mqtt_svc_close_owner(ctx->network_owner) == -EAGAIN) k_sleep(K_MSEC(1));
#endif
#if defined(CONFIG_ASTER_HTTP)
    while (aster_http_svc_close_owner(ctx->network_owner) == -EAGAIN) k_sleep(K_MSEC(1));
#endif
    while (aster_net_close_owner(ctx->network_owner) == -EAGAIN) k_sleep(K_MSEC(1));
    /* N5.5: pending state events must not outlive the session. */
    aster_net_event_drop_owner(ctx->network_owner);
    ctx->network_owner = 0;
}
#else
#define NET_STUB(name, args) static int32_t name args { return ASTER_ENOTSUP; }
NET_STUB(n_network_get_info, (wasm_exec_env_t env, uint32_t o, uint32_t n))
NET_STUB(n_network_open, (wasm_exec_env_t env, uint32_t k, uint32_t o, uint32_t n, uint32_t p, uint32_t t))
NET_STUB(n_network_send, (wasm_exec_env_t env, uint32_t h, uint32_t o, uint32_t n, uint32_t a, uint32_t b, uint32_t p))
NET_STUB(n_network_recv, (wasm_exec_env_t env, uint32_t h, uint32_t o, uint32_t n, uint32_t p, uint32_t s))
NET_STUB(n_network_status, (wasm_exec_env_t env, uint32_t h, uint32_t o, uint32_t n))
NET_STUB(n_network_cancel, (wasm_exec_env_t env, uint32_t h))
NET_STUB(n_network_close, (wasm_exec_env_t env, uint32_t h))
void aster_native_release_network(aster_app_ctx_t *ctx) { (void)ctx; }
void aster_net_pub_drop(uint64_t owner) { (void)owner; }
uint32_t aster_net_public_handle(uint64_t o, uint32_t k, uint32_t i)
{ (void)o; (void)k; (void)i; return 0; }
#undef NET_STUB
#endif
