/* SPDX-License-Identifier: MIT
 * Bounded mDNS publisher (RFC 6762 name subset). Runs inside the existing
 * HTTP worker tick (like WebSocket/MQTT): no new thread, no new timer, one
 * system-owned UDP socket. Publish is requested/withdrawn by owner bits (the
 * Wi-Fi settings UI, or the diagnostic CLI); while OFF nothing is allocated
 * and no socket exists. Question parsing is non-recursive and length-checked;
 * every response carries at most one A record.
 */
#include "aster_mdns.h"
#include "aster_http.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifndef ASTER_MDNS_HOST_TEST
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#endif

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define MDNS_RX_MAX 512
#define MDNS_TX_MAX 512
#define MDNS_TTL 120
#define MDNS_ANNOUNCES 2
#define MDNS_ANNOUNCE_GAP_MS 1000
K_MUTEX_DEFINE(mdns_lock);
static struct {
    uint32_t want, state, conflicts, renames, rx, tx, drops, announce;
    int32_t error;
    int fd;
    uint8_t suffix; /* 0 base; 2..9 after one passive conflict */
    int64_t next_action;
    struct net_if *iface;
    char name[ASTER_MDNS_NAME_MAX + 1];
    struct in_addr ip;
} m = { .fd = -1 }; /* static zero would look like a valid descriptor */

static void full_name(void)
{
    struct net_if *iface = net_if_get_default();
    struct net_linkaddr *ll = iface ? net_if_get_link_addr(iface) : NULL;
    char base[ASTER_MDNS_NAME_MAX - 6]; /* room for ".local" */
    m.name[0] = 0;
    if (!ll || ll->len < 3u || ll->len > sizeof(ll->addr)) return;
    const uint8_t *a = ll->addr;
    int n = snprintf(base, sizeof(base), "aster-%02x%02x%02x", a[ll->len - 3],
                     a[ll->len - 2], a[ll->len - 1]);
    if (n <= 0 || (size_t)n >= sizeof(base)) return;
    if (m.suffix) {
        size_t l = strlen(base);
        if (l + 2 >= sizeof(base)) return;
        base[l] = '-';
        base[l + 1] = (char)('0' + m.suffix);
        base[l + 2] = 0;
    }
    snprintf(m.name, sizeof(m.name), "%s.local", base);
}
/* dotted -> wire labels ending at root; false when over budget/empty label. */
static bool wire_name(const char *d, uint8_t *out, size_t cap, size_t *len)
{
    size_t o = 0, n = strlen(d), start = 0;
    if (!n || n > ASTER_MDNS_NAME_MAX) return false;
    for (size_t i = 0;; ++i) {
        if (i != n && d[i] != '.') continue;
        size_t l = i - start;
        if (!l || l > 63 || o + 1 + l > cap) return false;
        out[o++] = (uint8_t)l;
        memcpy(out + o, d + start, l);
        o += l;
        start = i + 1;
        if (i == n) break;
    }
    if (o + 1 > cap) return false;
    out[o++] = 0;
    *len = o;
    return true;
}
/* one question name -> dotted lowercase, no compression (returns -1 on ptr);
 * fills *consumed with wire bytes including the terminating root label. */
static int read_name(const uint8_t *p, size_t n, size_t at, char *out, size_t cap,
                     size_t *consumed)
{
    size_t o = 0, i = at;
    out[0] = 0;
    for (;;) {
        if (i >= n) return -1;
        uint8_t l = p[i];
        if (l & 0xc0) return -1;
        if (!l) { i++; break; }
        i++;
        if (i + l > n || o + l + 2 > cap) return -1;
        for (uint8_t k = 0; k < l; ++k) {
            uint8_t c = p[i + k];
            out[o++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
        }
        out[o++] = '.';
        i += l;
    }
    if (o) o--;
    out[o] = 0;
    *consumed = i - at;
    return 0;
}
static bool case_eq(const char *a, const char *b)
{
    for (size_t i = 0;; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
        if (!x) return true;
    }
}
static bool join_group(bool add)
{
    /* This Zephyr's IP_ADD_MEMBERSHIP requires struct net_ip_mreqn (12B):
     * group address + local interface address + interface index. The classic
     * 8-byte ip_mreq would be rejected with EINVAL. Pin the default (station)
     * interface by index so the IGMP join/leave cannot pick the wrong iface. */
    struct net_if *iface = net_if_get_default();
    struct net_ip_mreqn req;
    memset(&req, 0, sizeof(req));
    if (zsock_inet_pton(AF_INET, MDNS_GROUP, &req.imr_multiaddr) != 1) return false;
    if (iface) req.imr_ifindex = net_if_get_by_iface(iface);
    return zsock_setsockopt(m.fd, IPPROTO_IP,
                            add ? ZSOCK_IP_ADD_MEMBERSHIP : ZSOCK_IP_DROP_MEMBERSHIP,
                            &req, sizeof(req)) == 0;
}
static int open_socket(void)
{
    int fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -errno;
    int on = 1;
    (void)zsock_setsockopt(fd, SOL_SOCKET, ZSOCK_SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
    if (zsock_bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        int e = -errno;
        zsock_close(fd);
        return e;
    }
    m.fd = fd;
    if (!join_group(true)) {
        int e = errno ? -errno : -ENODEV;
        zsock_close(m.fd);
        m.fd = -1;
        return e;
    }
    /* Egress for the announce multicast: pin the station interface by index.
     * Without this Zephyr may pick the wrong route for the group address. */
    struct net_if *oif = net_if_get_default();
    struct net_ip_mreqn out;
    memset(&out, 0, sizeof(out));
    if (oif) out.imr_ifindex = net_if_get_by_iface(oif);
    uint8_t ttl = 255;
    (void)zsock_setsockopt(m.fd, IPPROTO_IP, ZSOCK_IP_MULTICAST_IF, &out, sizeof(out));
    (void)zsock_setsockopt(m.fd, IPPROTO_IP, ZSOCK_IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    return 0;
}
static void close_socket(void)
{
    if (m.fd >= 0) {
        join_group(false);
        zsock_close(m.fd);
        m.fd = -1;
    }
    m.iface = NULL;
}
static void put_a_rr(uint8_t *o, size_t *p, size_t cap, bool name_ptr, size_t name_off)
{
    size_t at = *p;
    if (name_ptr) {
        if (at + 2 > cap) return;
        o[at++] = 0xc0;
        o[at++] = (uint8_t)name_off;
    } else {
        size_t l;
        if (!wire_name(m.name, o + at, cap - at, &l)) return;
        at += l;
    }
    if (at + 10 + 4 > cap) return;
    o[at++] = 0; o[at++] = 1;   /* TYPE A */
    o[at++] = 0; o[at++] = 1;   /* CLASS IN */
    o[at++] = 0; o[at++] = 0; o[at++] = 0; o[at++] = MDNS_TTL; /* TTL */
    o[at++] = 0; o[at++] = 4;   /* RDLENGTH */
    memcpy(o + at, &m.ip, 4);
    at += 4;
    *p = at;
}
static void put_header(uint8_t *o, uint16_t id, bool aa, uint16_t qd, uint16_t an)
{
    o[0] = (uint8_t)(id >> 8); o[1] = (uint8_t)id;
    o[2] = (uint8_t)(0x80 | (aa ? 0x04 : 0)); o[3] = 0; /* QR|AA */
    o[4] = (uint8_t)(qd >> 8); o[5] = (uint8_t)qd;
    o[6] = (uint8_t)(an >> 8); o[7] = (uint8_t)an;
    o[8] = 0; o[9] = 0; o[10] = 0; o[11] = 0; /* NSCOUNT/ARCOUNT */
}
/* announce: answer-only record set on the multicast group. */
static void announce(void)
{
    uint8_t o[MDNS_TX_MAX];
    size_t at = 12;
    put_header(o, 0, true, 0, 1);
    put_a_rr(o, &at, sizeof(o), false, 0);
    if (at <= 12) return;
    struct sockaddr_in d = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
    zsock_inet_pton(AF_INET, MDNS_GROUP, &d.sin_addr);
    if (zsock_sendto(m.fd, o, at, MSG_DONTWAIT, (struct sockaddr *)&d, sizeof(d)) == (int)at)
        m.announce++;
}
/* reply: echo the single question and append our A answer (unicast). */
static size_t reply(const uint8_t *q, size_t qn, uint8_t *o)
{
    if (qn < 13) return 0;
    if ((q[2] & 0x80) || ((q[2] & 0x0f) != 0)) return 0; /* query, standard op */
    if (q[4] || q[5] != 1) return 0;                     /* exactly one question */
    if (q[6] || q[7] || q[8] || q[9] || q[10] || q[11]) return 0;
    char name[ASTER_MDNS_NAME_MAX + 1];
    size_t used = 0;
    if (read_name(q, qn, 12, name, sizeof(name), &used) < 0) return 0;
    size_t tail = 12 + used + 4;
    if (qn < tail) return 0;
    uint16_t qtype = (uint16_t)((q[tail - 4] << 8) | q[tail - 3]);
    uint16_t qclass = (uint16_t)((q[tail - 2] << 8) | q[tail - 1]);
    if (qclass != 1 && qclass != 0x8001) return 0;         /* IN or unicast-response */
    if (qtype != 1 && qtype != 0xff) return 0;             /* A or ANY */
    if (!case_eq(name, m.name)) return 0;                  /* foreign name: silent */
    if (12 + used + 4 + 16 > MDNS_TX_MAX) return 0; /* no room for the answer */
    uint16_t id = (uint16_t)((q[0] << 8) | q[1]);
    put_header(o, id, true, 1, 1);
    size_t at = 12;
    memcpy(o + 12, q + 12, used + 4);
    at += used + 4;
    put_a_rr(o, &at, MDNS_TX_MAX, true, 12); /* name pointer -> question name */
    return at > 12 + used + 4 ? at : 0; /* refuse to claim an answer that fit */
}
/* Busy contract: while a publish is wanted and not terminal, keep the worker
 * polling (WAIT_IP relies on it to catch a later DHCP lease without a timer).
 * OFF and FAILED return false so the worker parks on K_FOREVER at zero cost. */
bool aster_mdns_svc_tick(void)
{
    bool busy;
    if (!m.want) return false;
    k_mutex_lock(&mdns_lock, K_FOREVER);
    busy = m.state != ASTER_MDNS_FAILED;
    if (busy) {
        int64_t now = k_uptime_get();
        const struct in_addr *addr = NULL;
        struct net_if *iface = net_if_get_default();
        if (iface) addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
        if (!addr) {
            close_socket();
            m.state = ASTER_MDNS_WAIT_IP;
            memset(&m.ip, 0, sizeof(m.ip)); /* do not advertise a stale address */
        } else {
            bool moved = !m.iface || iface != m.iface || memcmp(&m.ip, addr, sizeof(m.ip));
            m.ip = *addr;
            if (m.fd < 0 || moved) {
                close_socket();
                if (!m.name[0]) full_name();
                if (!m.name[0]) {
                    m.error = -EINVAL;
                    m.state = ASTER_MDNS_FAILED;
                    busy = false;
                    close_socket();
                } else if ((m.error = open_socket()) < 0) {
                    m.state = ASTER_MDNS_WAIT_IP; /* transient; retry next tick */
                    /* m.error keeps the bind/join errno (already negated) */
                } else {
                    m.iface = iface;
                    m.error = 0;
                    m.state = ASTER_MDNS_ANNOUNCING;
                    m.announce = 0;
                    m.next_action = now;
                }
            }
            if (m.state == ASTER_MDNS_ANNOUNCING) {
                if (now >= m.next_action) {
                    announce();
                    m.announce++;
                    m.next_action = now + MDNS_ANNOUNCE_GAP_MS;
                    if (m.announce >= MDNS_ANNOUNCES) m.state = ASTER_MDNS_IDLE;
                }
            } else if (m.state == ASTER_MDNS_IDLE) {
                uint8_t q[MDNS_RX_MAX];
                for (int i = 0; i < 4; ++i) {
                    struct sockaddr_in from;
                    socklen_t fl = sizeof(from);
                    ssize_t got = zsock_recvfrom(m.fd, q, sizeof(q),
                                                 MSG_DONTWAIT | MSG_TRUNC,
                                                 (struct sockaddr *)&from, &fl);
                    if (got <= 0) break;
                    m.rx++;
                    if (got > (ssize_t)sizeof(q)) { m.drops++; continue; }
                    char name[ASTER_MDNS_NAME_MAX + 1];
                    size_t used = 0;
                    if (q[2] & 0x80) { /* answer: only our own name matters */
                        if ((q[6] || q[7]) &&
                            read_name(q, (size_t)got, 12, name, sizeof(name), &used) == 0 &&
                            case_eq(name, m.name) &&
                            memcmp(&from.sin_addr, &m.ip, sizeof(m.ip))) {
                            m.conflicts++;
                            close_socket();
                            if (!m.suffix) {
                                m.suffix = 2;
                                m.renames = 1;
                                m.name[0] = 0;
                                m.state = ASTER_MDNS_WAIT_IP;
                            } else {
                                m.error = -EEXIST;
                                m.state = ASTER_MDNS_FAILED;
                                busy = false;
                            }
                            break;
                        }
                        continue; /* self echo or foreign name: ignore */
                    }
                    uint8_t o[MDNS_TX_MAX];
                    size_t rl = reply(q, (size_t)got, o);
                    if (!rl) continue;
                    /* Unicast back to the querier's real source port (mDNS
                     * peers use 5353, but test/ephemeral clients use a random
                     * port; forcing 5353 would strand the reply). */
                    if (zsock_sendto(m.fd, o, rl, MSG_DONTWAIT,
                                     (struct sockaddr *)&from, sizeof(from)) == (int)rl)
                        m.tx++;
                }
            }
        }
    }
    k_mutex_unlock(&mdns_lock);
    return busy;
}
int aster_mdns_svc_acquire(uint32_t bit)
{
    int rc = 0;
    if (bit != ASTER_MDNS_OWNER_UI && bit != ASTER_MDNS_OWNER_CLI) return -EINVAL;
    k_mutex_lock(&mdns_lock, K_FOREVER);
    bool fresh = !(m.want & bit) && !m.want;
    m.want |= bit;
    /* Re-acquire after a terminal FAILED restarts the session (e.g. CLI off/on
     * or reopening the settings page); otherwise FAILED would latch. */
    if (!fresh && m.state == ASTER_MDNS_FAILED) fresh = true;
    if (fresh) {
        m.state = ASTER_MDNS_WAIT_IP;
        m.error = 0;
        m.suffix = 0;
        m.name[0] = 0;
        m.fd = -1; /* static zero would look like a valid descriptor */
        m.rx = m.tx = m.drops = m.announce = m.conflicts = m.renames = 0;
    }
    k_mutex_unlock(&mdns_lock);
    if (fresh) {
        /* Ensure the shared worker exists and is signalled; a lost wake would
         * strand the publish with nobody ticking. */
        for (int i = 0; i < 20 && (rc = aster_http_svc_worker_wake()) == -EAGAIN; ++i)
            ;
        if (rc) {
            k_mutex_lock(&mdns_lock, K_FOREVER);
            m.want &= ~bit;
            if (!m.want) {
                m.state = ASTER_MDNS_OFF;
                m.error = rc;
                m.name[0] = 0;
            }
            k_mutex_unlock(&mdns_lock);
        }
    }
    return rc;
}
int aster_mdns_svc_release(uint32_t bit)
{
    if (bit != ASTER_MDNS_OWNER_UI && bit != ASTER_MDNS_OWNER_CLI) return -EINVAL;
    k_mutex_lock(&mdns_lock, K_FOREVER);
    m.want &= ~bit;
    if (!m.want) {
        close_socket();
        m.state = ASTER_MDNS_OFF;
        m.error = 0;
        m.suffix = 0;
        m.name[0] = 0;
        memset(&m.ip, 0, sizeof(m.ip));
        m.rx = m.tx = m.drops = m.announce = m.conflicts = m.renames = 0;
    }
    k_mutex_unlock(&mdns_lock);
    return 0;
}
int aster_mdns_svc_status(struct aster_mdns_snapshot *out)
{
    if (!out) return -EINVAL;
    if (k_mutex_lock(&mdns_lock, K_NO_WAIT)) return -EAGAIN;
    memset(out, 0, sizeof(*out));
    out->state = m.state;
    out->want = m.want;
    out->conflicts = m.conflicts;
    out->renames = m.renames;
    out->error = m.error;
    out->rx = m.rx;
    out->tx = m.tx;
    out->drops = m.drops;
    out->announce = m.announce;
    memcpy(out->name, m.name, strlen(m.name) + 1);
    if (m.ip.s_addr) zsock_inet_ntop(AF_INET, &m.ip, out->ipv4, sizeof(out->ipv4));
    k_mutex_unlock(&mdns_lock);
    return 0;
}
