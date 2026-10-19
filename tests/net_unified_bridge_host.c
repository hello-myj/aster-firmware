/* SPDX-License-Identifier: MIT: ABI 4 unified imports over the TCP/UDP service. */
#define ASTER_NET_HOST_TEST
#include "../systems/zephyr/services/aster_net.c"
#include "../systems/zephyr/services/aster_net_event.c"
#include <aster/aster.h>
#include "../systems/zephyr/services/aster_perms.h"
#define CONFIG_ASTER_NET 1
#define IS_ENABLED(n) (n)
#define zsock_inet_ntop inet_ntop
static unsigned slept;
static void k_sleep(int n) { slept += n; net_lock = 0; }
typedef struct aster_app_ctx { uint64_t network_owner; uint32_t permissions; } aster_app_ctx_t;
typedef struct { aster_app_ctx_t ctx; uint8_t memory[8192]; } guest;
typedef guest *wasm_exec_env_t;
typedef guest *wasm_module_inst_t;
static aster_app_ctx_t *ctx_of(guest *g) { return &g->ctx; }
static bool aster_ctx_perm(aster_app_ctx_t *ctx, uint32_t bit) { return !!(ctx->permissions & bit); }
static guest *wasm_runtime_get_module_inst(guest *g) { return g; }
static bool wasm_runtime_get_app_addr_range(guest *g, uint32_t off, void *start, uint64_t *end)
{ (void)g; (void)start; *end = 8192; return off < 8192; }
static void *wasm_runtime_addr_app_to_native(guest *g, uint32_t off) { return g->memory + off; }
#include "../systems/zephyr/runtime/aster_network_bridge.inc"
#include "../systems/zephyr/runtime/aster_net_bridge.inc"
static void tick(void) { host_now++; service_tick(); }
static void put_open(guest *g, uint32_t proto, uint32_t port, uint32_t timeout, uint32_t toff, uint32_t tlen)
{
    aster_net_open_hdr_t h = { .size = sizeof(h), .version = 1, .proto = proto,
        .timeout_ms = timeout, .port = port, .target = { toff, tlen } };
    memcpy(g->memory + 300, &h, sizeof(h));
}
int main(void)
{
    guest a = {0}, b = {0};
    (void)n_network_recv; (void)n_network_status;
    assert(n_net_info(&a, 100, sizeof(aster_net_info_t)) == ASTER_EPERM);
    assert(n_net_open(&a, 300, sizeof(aster_net_open_hdr_t)) == ASTER_EPERM);
    a.ctx.permissions = b.ctx.permissions = ASTER_PERM_NET;
    assert(n_net_open(&a, 300, sizeof(aster_net_open_hdr_t) - 1) == ASTER_EINVAL);
    put_open(&a, 9, 0, 0, 0, 0);
    assert(n_net_open(&a, 300, sizeof(aster_net_open_hdr_t)) == ASTER_ENOTSUP);
    ((aster_net_open_hdr_t *)(a.memory + 300))->version = 2;
    assert(n_net_open(&a, 300, sizeof(aster_net_open_hdr_t)) == ASTER_EINVAL);
    put_open(&a, ASTER_NET_PROTO_UDP, 0, 0, 0, 0);
    int h = n_net_open(&a, 300, sizeof(aster_net_open_hdr_t));
    assert(h > 0); tick();
    put_open(&b, ASTER_NET_PROTO_UDP, 0, 0, 0, 0);
    int other = n_net_open(&b, 300, sizeof(aster_net_open_hdr_t));
    assert(other > 0); tick();
    put_open(&a, ASTER_NET_PROTO_UDP, 0, 0, 0, 0);
    assert(n_net_open(&a, 300, sizeof(aster_net_open_hdr_t)) == ASTER_ENOSPC);
    assert(n_net_close(&a, other) == ASTER_EBADF);
    assert(n_net_status(&a, h, 500, 71) == ASTER_EINVAL);
    assert(!n_net_status(&b, other, 500, sizeof(aster_net_status_hdr_t) + sizeof(aster_net_tcp_status_t)));
    aster_net_status_hdr_t st; memcpy(&st, b.memory + 500, sizeof(st));
    assert(st.proto == ASTER_NET_PROTO_UDP && st.state_class == ASTER_NET_CLASS_READY);
    aster_net_io_t io = { .size = sizeof(io), .version = 1 };
    memcpy(a.memory + 700, &io, sizeof(io));
    assert(n_net_io(&a, h, 99, 700, sizeof(io)) == ASTER_ENOTSUP);
    assert(!n_net_cancel(&a, h));
    assert(!n_net_status(&a, h, 500, sizeof(aster_net_status_hdr_t) + sizeof(aster_net_tcp_status_t)));
    memcpy(&st, a.memory + 500, sizeof(st));
    assert(st.state_class == ASTER_NET_CLASS_TERMINAL && st.proto_state == ASTER_NETWORK_CANCELLED);
    assert(!n_net_close(&a, h));
    assert(n_net_close(&a, h) == ASTER_EBADF);
    assert(n_net_status(&b, other, 500, 72) == ASTER_EBADF || !n_net_close(&b, other));
    aster_native_release_network(&a.ctx);
    aster_native_release_network(&b.ctx);
    tick();
    assert(!psram_live);
    return 0;
}
