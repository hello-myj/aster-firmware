/* SPDX-License-Identifier: MIT. Actual native WS bridge over the real service
 * and POSIX loopback: guest address/length validation, copy-before-return,
 * stable error domain, ownership, quota refusal and APP-exit cleanup. */
#define main ws_service_regression_main
#include "ws_service_host.c"
#undef main
#include <aster/aster.h>
#include "../systems/zephyr/services/aster_perms.h"
#define CONFIG_ASTER_NET 1
#define CONFIG_ASTER_HTTP 1
#define IS_ENABLED(n) (n)
#define zsock_inet_ntop inet_ntop
static void k_sleep(int n) { host_now += n; }
typedef struct aster_app_ctx { uint64_t network_owner; uint32_t permissions; } aster_app_ctx_t;
typedef struct { aster_app_ctx_t ctx; uint8_t memory[8192]; } guest;
typedef guest *wasm_exec_env_t;
typedef guest *wasm_module_inst_t;
static aster_app_ctx_t *ctx_of(guest *g) { return &g->ctx; }
static bool aster_ctx_perm(aster_app_ctx_t *c, uint32_t b) { return !!(c->permissions & b); }
static guest *wasm_runtime_get_module_inst(guest *g) { return g; }
static bool wasm_runtime_get_app_addr_range(guest *g, uint32_t o, void *s, uint64_t *e)
{ (void)g; (void)s; *e = 8192; return o < 8192; }
static void *wasm_runtime_addr_app_to_native(guest *g, uint32_t o) { return g->memory + o; }
#include "../systems/zephyr/runtime/aster_network_bridge.inc"
#include "../systems/zephyr/runtime/aster_http_bridge.inc"
#include "../systems/zephyr/runtime/aster_ws_bridge.inc"

static int bridge_open(wasm_exec_env_t env)
{
    char url[64];
    int n = snprintf(url, sizeof(url), "ws://127.0.0.1:%u/echo", (unsigned)port);
    guest *g = env;
    memcpy(g->memory + 200, url, n);
    int32_t h = n_ws_connect(env, 200, (uint32_t)n, 10000);
    assert(h > 0);
    return h;
}
static void settle(void)
{
    for (unsigned i = 0; i < 4000 && ws_client; i++) pump();
    if (ws_peer >= 0) { close(ws_peer); ws_peer = -1; }
    for (unsigned i = 0; i < 100; i++) pump();
    assert(!psram_live);
}

int main(void)
{
    (void)n_network_get_info; (void)n_network_open; (void)n_network_send;
    (void)n_network_recv; (void)n_network_status; (void)n_network_cancel; (void)n_network_close;
    (void)n_http_request; (void)n_http_status; (void)n_http_read; (void)n_http_header;
    (void)n_http_cancel; (void)n_http_close;
    signal(SIGPIPE, SIG_IGN);
    server = socket(AF_INET, SOCK_STREAM, 0); assert(server >= 0);
    struct sockaddr_in a4 = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    assert(!bind(server, (void *)&a4, sizeof(a4))); assert(!listen(server, 4));
    fcntl(server, F_SETFL, O_NONBLOCK);
    socklen_t len = sizeof(a4);
    assert(!getsockname(server, (void *)&a4, &len)); port = ntohs(a4.sin_port);

    /* Private errno never leaks: certificates map ECERT, framing/UTF-8 to
     * EPROTO and a drained closed client reports stable EPIPE. */
    struct aster_ws_snapshot cert = { .tls_verify = 4 };
    assert(ws_error(-EACCES, &cert) == ASTER_ECERT);
    assert(ws_error(-EACCES, NULL) == ASTER_EPERM);
    assert(ws_error(-EPROTO, NULL) == ASTER_EPROTO);
    assert(ws_error(-EILSEQ, NULL) == ASTER_EPROTO);
    assert(ws_error(-ENODATA, NULL) == ASTER_EPIPE);

    guest a = { 0 }, b = { 0 };
    assert(n_ws_connect(&a, 200, 9, 10000) == ASTER_EPERM);
    a.ctx.permissions = ASTER_PERM_WIFI;
    assert(n_ws_recv(&a, 1, 1001, 4124) == ASTER_EPERM);
    a.ctx.permissions = b.ctx.permissions = ASTER_PERM_NET;

    /* Guest input validation happens before the service is consulted. */
    memcpy(a.memory + 200, "ws://1/1", 8);
    assert(n_ws_connect(&a, 200, 0, 10000) == ASTER_EINVAL);
    assert(n_ws_connect(&a, 8190, 9, 10000) == ASTER_EINVAL);
    assert(n_ws_connect(&a, 200, 1025, 10000) == ASTER_EINVAL);
    a.memory[204] = 0;
    assert(n_ws_connect(&a, 200, 8, 10000) == ASTER_EINVAL); /* embedded NUL */
    a.memory[204] = 'l';
    assert(n_ws_connect(&a, 200, 8, 50) == ASTER_EINVAL); /* timeout below 100 */
    assert(n_ws_connect(&a, 200, 8, 30001) == ASTER_EINVAL);

    int h = bridge_open(&a);
    memset(a.memory + 200, 0, 64); /* URL copied: erasing cannot break it */
    upgrade();
    struct aster_ws_snapshot s;
    assert(aster_ws_svc_status(2, h, &s) == 0 && s.state == ASTER_WS_READY);
    assert(n_ws_status(&b, h, 501, 48) == ASTER_EBADF); /* foreign APP owner */
    assert(n_ws_status(&a, 0, 501, 48) == ASTER_EBADF); /* handle 0 never exists */
    assert(n_ws_status(&a, h, 8190, 48) == ASTER_EINVAL);
    assert(n_ws_status(&a, h, 501, 47) == ASTER_EINVAL);
    assert(!n_ws_status(&a, h, 501, 52));
    aster_ws_status_t st; memcpy(&st, a.memory + 501, sizeof(st));
    assert(st.size == 48 && st.version == 1 && st.state == ASTER_WS_STATE_READY);
    assert(!st.error && !st.queued && !st.pending && !st.received && !st.sent);
    assert(!st.close_code && !st.certificate_flags && !st.reserved[0] && !st.reserved[1]);

    /* send validation without touching the bounded slot. */
    assert(n_ws_send(&a, h, 0, 0, 0) == ASTER_EINVAL);
    assert(n_ws_send(&a, h, 3, 200, 1) == ASTER_EINVAL);
    assert(n_ws_send(&a, h, 2, 200, 4097) == ASTER_EMSGSIZE);
    assert(n_ws_send(&a, h, 2, 300, 0) == ASTER_EINVAL); /* NULL required when empty */
    assert(n_ws_send(&a, h, 2, 8190, 4) == ASTER_EINVAL);

    /* One empty text message round-trips; length 0 is distinct from EAGAIN. */
    assert(!n_ws_send(&a, h, 1, 0, 0));
    expect_client(1, NULL, 0);
    const uint8_t empty_text[] = { 0x81, 0 };
    wire(empty_text, sizeof(empty_text));
    memset(a.memory + 1001 + offsetof(aster_ws_message_t, data), 0xEE, 64);
    assert(n_ws_recv(&a, h, 8190, 4124) == ASTER_EINVAL); /* destination range */
    assert(n_ws_recv(&a, h, 1001, 4123) == ASTER_EINVAL); /* short struct */
    assert(!n_ws_recv(&a, h, 1001, 4130));
    aster_ws_message_t m; memcpy(&m, a.memory + 1001, sizeof(m));
    assert(m.size == 4124 && m.version == 1 && m.opcode == ASTER_WS_OPCODE_TEXT);
    assert(m.length == 0 && !m.reserved[0] && !m.reserved[1] && !m.reserved[2]);
    assert(m.data[0] == 0xEE); /* untouched tail: empty is a real message */
    assert(n_ws_recv(&a, h, 1001, 4124) == ASTER_EAGAIN);

    /* Payload copies at call time: later guest edits must not leak in. */
    memcpy(a.memory + 200, "abc", 3);
    assert(!n_ws_send(&a, h, 1, 200, 3));
    memset(a.memory + 200, 0x5A, 3);
    expect_client(1, (const uint8_t *)"abc", 3);
    const uint8_t echo_text[] = { 0x81, 3, 'a', 'b', 'c' };
    wire(echo_text, sizeof(echo_text));
    memset(a.memory + 1001 + offsetof(aster_ws_message_t, data), 0xEE, 64);
    assert(!n_ws_recv(&a, h, 1001, 4124));
    memcpy(&m, a.memory + 1001, sizeof(m));
    assert(m.length == 3 && !memcmp(m.data, "abc", 3));
    assert(m.data[3] == 0xEE && m.data[63] == 0xEE);

    /* Server-pushed ordered messages keep boundaries; invalid recv preserves. */
    const uint8_t qa[] = { 0x82, 1, 0x41 }, qb[] = { 0x82, 1, 0x42 };
    wire(qa, sizeof(qa)); wire(qb, sizeof(qb)); steps(100);
    assert(!n_ws_status(&a, h, 501, 48)); memcpy(&st, a.memory + 501, sizeof(st));
    assert(st.queued == 1 && st.received == 3); /* qb held: one bounded slot */
    assert(!n_ws_recv(&a, h, 1001, 4124)); memcpy(&m, a.memory + 1001, sizeof(m));
    assert(m.length == 1 && m.data[0] == 0x41 && m.opcode == ASTER_WS_OPCODE_BINARY);
    steps(100); /* worker assembles the queued second message */
    assert(!n_ws_recv(&a, h, 1001, 4124)); memcpy(&m, a.memory + 1001, sizeof(m));
    assert(m.length == 1 && m.data[0] == 0x42);
    assert(n_ws_recv(&a, h, 1001, 4124) == ASTER_EAGAIN);

    /* Protocol fault maps to one stable domain and remains on recv/status. */
    const uint8_t masked_from_peer[] = { 0x81, 0x80, 0, 0, 0, 0 };
    wire(masked_from_peer, sizeof(masked_from_peer));
    assert(!n_ws_status(&a, h, 501, 48)); memcpy(&st, a.memory + 501, sizeof(st));
    assert(st.state == ASTER_WS_STATE_FAILED && st.error == ASTER_EPROTO);
    assert(n_ws_recv(&a, h, 1001, 4124) == ASTER_EPROTO);
    assert(!n_ws_close(&a, h));
    settle();

    /* Peer close: buffered input precedes the terminal drained state. */
    h = bridge_open(&a); upgrade();
    const uint8_t kept[] = { 0x82, 1, 0x49 };
    wire(kept, sizeof(kept)); steps(50);
    const uint8_t bye[] = { 0x88, 2, 3, 232 };
    wire(bye, sizeof(bye)); steps(50);
    assert(!n_ws_recv(&a, h, 1001, 4124)); memcpy(&m, a.memory + 1001, sizeof(m));
    assert(m.length == 1 && m.data[0] == 0x49);
    assert(n_ws_recv(&a, h, 1001, 4124) == ASTER_EPIPE);
    assert(!n_ws_status(&a, h, 501, 48)); memcpy(&st, a.memory + 501, sizeof(st));
    assert(st.state == ASTER_WS_STATE_CLOSED && st.close_code == 1000 && !st.error);
    assert(!n_ws_close(&a, h));
    settle();

    /* cancel keeps the handle and a stable CANCELLED terminal until close;
     * a completed buffered message is delivered before the terminal error. */
    h = bridge_open(&a); upgrade();
    const uint8_t pre[] = { 0x82, 1, 0x4F };
    wire(pre, sizeof(pre)); steps(50);
    assert(!n_ws_cancel(&a, h));
    steps(5);
    assert(!n_ws_recv(&a, h, 1001, 4124)); memcpy(&m, a.memory + 1001, sizeof(m));
    assert(m.length == 1 && m.data[0] == 0x4F); /* completed first, then terminal */
    assert(n_ws_recv(&a, h, 1001, 4124) == ASTER_ECANCELLED);
    assert(!n_ws_status(&a, h, 501, 48)); memcpy(&st, a.memory + 501, sizeof(st));
    assert(st.state == ASTER_WS_STATE_CANCELLED && st.error == ASTER_ECANCELLED);
    /* Post-terminal send is refused; not-cancelled-not-ready maps ECONNRESET,
     * matching HTTP/MQTT where only READY accepts a new message. */
    assert(n_ws_send(&a, h, 2, 0, 0) == ASTER_ECONNRESET);
    assert(!n_ws_close(&a, h));
    settle();

    /* Single-client quota: a second APP is refused without touching slots,
     * and recovery works after the holder closes. */
    int h2 = bridge_open(&b); upgrade();
    memcpy(a.memory + 200, "ws://127.0.0.1:9/echo", 21);
    assert(n_ws_connect(&a, 200, 21, 10000) == ASTER_ENOSPC);
    assert(!n_ws_close(&b, h2));
    settle();

    /* Stop/trap/init-failure cleanup path used by the runtime: WS closes
     * with the session owner; the next session gets a fresh owner so old
     * handles can never be addressed again. */
    h = bridge_open(&a); upgrade();
    uint64_t retired = a.ctx.network_owner;
    assert(ws_client && ws_client->owner == retired);
    aster_native_release_network(&a.ctx);
    assert(!a.ctx.network_owner); /* session retired on stop/trap */
    settle();
    /* A new call mints a fresh owner; the old handle stays permanently dead. */
    assert(n_ws_status(&a, h, 501, 48) == ASTER_EBADF);
    assert(a.ctx.network_owner && a.ctx.network_owner != retired);

    close(server);
    puts("WS bridge guest boundaries, copy, ownership and exit cleanup OK");
    return 0;
}
