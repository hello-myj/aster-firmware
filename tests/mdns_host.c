/* SPDX-License-Identifier: MIT
 * Host harness for the actual mDNS wire codec (ASan/UBSan). Drives the static
 * question parser, response builder and name helpers directly against valid,
 * truncated, compressed, oversized and random-prefix inputs. Sockets and the
 * shared HTTP worker are stubbed at the include boundary; the state machine's
 * socket/tick path is not exercised here (that is the hardware G1 gate).
 */
#define ASTER_MDNS_HOST_TEST
#include <errno.h>
#include "net_host_platform.h"
#include <netinet/in.h>
#include <sys/socket.h>
/* mDNS-specific shims the shared net platform header does not declare. */
#define net_in_addr in_addr
#define zsock_inet_ntop inet_ntop
struct net_ip_mreqn {
    struct net_in_addr imr_multiaddr;
    struct net_in_addr imr_address;
    int imr_ifindex;
};
#ifndef ZSOCK_SO_REUSEADDR
#define ZSOCK_SO_REUSEADDR 2
#endif
#ifndef ZSOCK_IP_ADD_MEMBERSHIP
#define ZSOCK_IP_ADD_MEMBERSHIP 35
#endif
#ifndef ZSOCK_IP_DROP_MEMBERSHIP
#define ZSOCK_IP_DROP_MEMBERSHIP 36
#endif
#ifndef ZSOCK_IP_MULTICAST_IF
#define ZSOCK_IP_MULTICAST_IF 32
#endif
#ifndef ZSOCK_IP_MULTICAST_TTL
#define ZSOCK_IP_MULTICAST_TTL 33
#endif
/* Mirrors Zephyr's real validator: this build only accepts mreqn (12B), the
 * group address must be set, and it records the last join for assertions. */
static int join_calls;
static struct net_ip_mreqn last_mreqn;
static int last_optname;
static int setsockopt_stub(int fd, int level, int opt, const void *v, int len)
{
    (void)fd; (void)level;
    if (opt == ZSOCK_IP_ADD_MEMBERSHIP || opt == ZSOCK_IP_DROP_MEMBERSHIP) {
        assert(level == IPPROTO_IP);
        if (len != (int)sizeof(struct net_ip_mreqn)) { errno = EINVAL; return -1; }
        last_mreqn = *(const struct net_ip_mreqn *)v;
        if (last_mreqn.imr_multiaddr.s_addr == 0) { errno = EINVAL; return -1; }
        last_optname = opt;
        if (opt == ZSOCK_IP_ADD_MEMBERSHIP) join_calls++;
        return 0;
    }
    return 0;
}
#define zsock_setsockopt setsockopt_stub
struct net_linkaddr { uint8_t type, len; uint8_t addr[8]; };
static struct net_linkaddr host_mac;
static struct net_linkaddr *net_if_get_link_addr(struct net_if *i)
{ (void)i; return host_mac.len ? &host_mac : NULL; }
static int net_if_get_by_iface(struct net_if *i) { return i ? 1 : 0; }
/* aster_http worker stubs: acquire only wakes; tick never runs here. */
static int http_wakes;
int aster_http_svc_worker_wake(void) { ++http_wakes; return 0; }
void aster_http_svc_worker_signal(void) {}
#include "../systems/zephyr/services/aster_mdns.c"

static size_t put_name(uint8_t *p, const char *dotted)
{
    size_t o = 0, start = 0, n = strlen(dotted);
    for (size_t i = 0;; ++i) {
        if (i != n && dotted[i] != '.') continue;
        size_t l = i - start;
        p[o++] = (uint8_t)l;
        memcpy(p + o, dotted + start, l);
        o += l;
        start = i + 1;
        if (i == n) break;
    }
    p[o++] = 0;
    return o;
}
/* single-question DNS query buffer; returns total length */
static size_t query(uint8_t *b, const char *name, uint16_t qtype)
{
    memset(b, 0, 12);
    b[4] = 0; b[5] = 1;
    size_t o = 12 + put_name(b + 12, name);
    b[o++] = (uint8_t)(qtype >> 8); b[o++] = (uint8_t)qtype;
    b[o++] = 0; b[o++] = 1; /* CLASS IN */
    return o;
}
/* question whose name is a compression pointer (0xc0) instead of labels */
static size_t query_ptr(uint8_t *b)
{
    memset(b, 0, 12);
    b[4] = 0; b[5] = 1;
    size_t o = 12;
    b[o++] = 0xc0; b[o++] = 12;
    b[o++] = 0; b[o++] = 1; /* TYPE A */
    b[o++] = 0; b[o++] = 1; /* CLASS IN */
    return o;
}
static void test_foreign_query(void)
{
    uint8_t q[512], o[512];
    size_t qn = query(q, "other.local", 1);
    assert(!reply(q, qn, o));
    q[2] = 0x80; /* answer bit set: not a query */
    assert(!reply(q, qn, o));
}
static void test_matching_query(void)
{
    uint8_t q[512], o[512];
    size_t qn = query(q, m.name, 1);
    size_t rl = reply(q, qn, o);
    assert(rl > qn);
    assert((o[2] & 0x84) == 0x84); /* QR|AA */
    assert(o[4] == 0 && o[5] == 1 && o[6] == 0 && o[7] == 1); /* QD=1 AN=1 */
    assert(rl >= 12 + 4 + 16);
    assert(o[rl - 16] == 0xc0); /* answer name -> question-name pointer */
    assert(o[rl - 6] == 0 && o[rl - 5] == 4); /* RDLENGTH=4 */
    assert(!memcmp(o + rl - 4, &m.ip, 4));
}
static void test_any_and_ptr(void)
{
    uint8_t q[512], o[512];
    size_t qn = query(q, m.name, 0xff); /* ANY on our name */
    assert(reply(q, qn, o) > qn);
    assert(!reply(q, query_ptr(q), o)); /* compression pointer rejected */
}
static void test_malformed(void)
{
    uint8_t q[512], o[512];
    assert(!reply(q, 0, o));
    assert(!reply(q, 12, o));
    size_t qn = query(q, m.name, 1);
    assert(!reply(q, qn - 1, o)); /* cut class octet */
    assert(!reply(q, 12, o)); /* header only */
    qn = query(q, m.name, 1); q[5] = 2;
    assert(!reply(q, qn, o)); /* QDCOUNT!=1 */
    qn = query(q, m.name, 1); q[qn - 1] = 3;
    assert(!reply(q, qn, o)); /* QCLASS=CH rejected */
    qn = query(q, m.name, 1); q[6] = 0; q[7] = 1;
    assert(!reply(q, qn, o)); /* ANCOUNT already set -> ignore */
}
/* Random + every-prefix truncation must never read/write out of bounds. */
static void test_fuzz_prefixes(void)
{
    uint8_t q[512], o[512];
    size_t full = query(q, m.name, 1);
    for (size_t len = 0; len <= full; ++len)
        assert(reply(q, len, o) == 0 || reply(q, len, o) > len);
    for (unsigned s = 1; s < 4096; ++s) {
        uint8_t buf[64];
        uint32_t x = s * 2654435761u;
        for (unsigned k = 0; k < sizeof(buf); ++k) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; buf[k] = (uint8_t)x; }
        /* never read past what we hand in */
        (void)reply(buf, 64, o);
    }
}
static void test_case_insensitive(void)
{
    uint8_t q[512], o[512];
    char up[ASTER_MDNS_NAME_MAX + 1];
    size_t i = 0;
    for (; m.name[i]; ++i) up[i] = (m.name[i] >= 'a' && m.name[i] <= 'z')
        ? (char)(m.name[i] - 32) : m.name[i];
    up[i] = 0;
    assert(reply(q, query(q, up, 1), o) > 0);
}
static void test_wire_name_bounds(void)
{
    uint8_t w[128];
    size_t len = 0;
    assert(wire_name("a.local", w, sizeof(w), &len) && len == 9 && w[0] == 1 && w[len - 1] == 0);
    assert(!wire_name("", w, sizeof(w), &len));
    assert(!wire_name(".lead.local", w, sizeof(w), &len));
    assert(!wire_name("trail.local.", w, sizeof(w), &len));
    char big[ASTER_MDNS_NAME_MAX + 8];
    memset(big, 'x', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
    assert(!wire_name(big, w, sizeof(w), &len));
    assert(!wire_name("aster-112233.local", w, 8, &len)); /* budget too small */
}
int main(void)
{
    /* The shared net platform header carries stubs this codec-only test does
     * not call; reference them so -Werror=unused-function stays honest. */
    (void)host_send; (void)host_recv; (void)dns_get_addr_info;
    (void)dns_cancel_addr_info_with_name; (void)k_thread_name_set;
    (void)k_thread_create; (void)k_sem_take; (void)k_sem_give;
    (void)net_if_is_up; (void)net_if_is_carrier_ok;
    host_mac.len = 6;
    memcpy(host_mac.addr, (uint8_t[]){0x12, 0x34, 0xaa, 0xbb, 0xcc, 0xdd}, 6);
    m.suffix = 0;
    full_name();
    printf("name=%s\n", m.name);
    assert(!strcmp(m.name, "aster-bbccdd.local")); /* last 3 MAC bytes */
    m.ip.s_addr = htonl(0xc0a80201u);
    m.state = ASTER_MDNS_IDLE;

    test_foreign_query();
    test_matching_query();
    test_any_and_ptr();
    test_malformed();
    test_fuzz_prefixes();
    test_case_insensitive();
    test_wire_name_bounds();

    m.suffix = 2;
    m.name[0] = 0;
    full_name();
    assert(!strcmp(m.name, "aster-bbccdd-2.local"));

    /* owner-bit accounting (socket path untouched; release of fd -1 is no-op) */
    assert(aster_mdns_svc_acquire(ASTER_MDNS_OWNER_UI) == 0);
    assert(m.want == ASTER_MDNS_OWNER_UI && http_wakes == 1);
    assert(aster_mdns_svc_acquire(ASTER_MDNS_OWNER_UI) == 0); /* no second wake */
    assert(http_wakes == 1);
    assert(aster_mdns_svc_acquire(ASTER_MDNS_OWNER_CLI) == 0);
    assert(m.want == (ASTER_MDNS_OWNER_UI | ASTER_MDNS_OWNER_CLI));
    assert(aster_mdns_svc_release(ASTER_MDNS_OWNER_UI) == 0);
    assert(m.want == ASTER_MDNS_OWNER_CLI);
    assert(aster_mdns_svc_release(ASTER_MDNS_OWNER_CLI) == 0);
    assert(m.want == 0 && m.state == ASTER_MDNS_OFF);
    assert(aster_mdns_svc_acquire(0) == -EINVAL);
    assert(aster_mdns_svc_release(99) == -EINVAL);

    /* Terminal FAILED must not latch: re-acquire restarts the session. */
    assert(aster_mdns_svc_acquire(ASTER_MDNS_OWNER_CLI) == 0);
    m.state = ASTER_MDNS_FAILED;
    m.error = -EEXIST;
    int wakes_before = http_wakes;
    assert(aster_mdns_svc_acquire(ASTER_MDNS_OWNER_UI) == 0);
    assert(http_wakes == wakes_before + 1 && m.state == ASTER_MDNS_WAIT_IP && !m.error);
    assert(aster_mdns_svc_release(ASTER_MDNS_OWNER_UI) == 0);
    assert(aster_mdns_svc_release(ASTER_MDNS_OWNER_CLI) == 0);

    struct aster_mdns_snapshot snap;
    assert(aster_mdns_svc_status(&snap) == 0);
    assert(snap.state == ASTER_MDNS_OFF && snap.want == 0);
    assert(aster_mdns_svc_status(NULL) == -EINVAL);

    puts("mDNS codec bounds, response build, fuzz and owner-bit accounting OK");
    return 0;
}
