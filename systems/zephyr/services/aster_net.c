/* SPDX-License-Identifier: MIT */
#include "aster_net.h"
#include "aster_mem.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#ifdef ASTER_NET_HOST_TEST
#include "net_host_platform.h"
#else
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/net/dns_resolve.h>
#endif

#define NET_STACK 4096
#define IO_TIMEOUT 5000
struct datagram {
    uint8_t data[ASTER_NET_UDP_MAX];
    uint32_t ip;
    uint16_t length, port;
};
struct outbound {
    struct datagram packet;
    char host[ASTER_NET_HOST_MAX + 1];
    int64_t deadline;
};
union buffers {
    struct { uint8_t tx[ASTER_NET_TCP_BUFFER], rx[ASTER_NET_TCP_BUFFER]; } tcp;
    struct { struct outbound tx[ASTER_NET_UDP_DEPTH];
             struct datagram rx[ASTER_NET_UDP_DEPTH + 1]; } udp;
};
struct slot {
    uint64_t owner;
    uint32_t handle;
    enum aster_net_kind kind;
    enum aster_net_state state;
    enum aster_net_phase phase;
    int error, fd;
    bool closing, cancel, dns_started, dns_done, addr_ready, restarting, retired;
    uint16_t dns_id, port, local_port;
    uint32_t ip;
    char host[ASTER_NET_HOST_MAX + 1];
    int64_t deadline, tx_deadline;
    uint16_t tx_len, rx_len;
    uint64_t tx_bytes, rx_bytes;
    uint32_t dropped;
    union buffers *buf;
};
static struct slot slots[ASTER_NET_SLOTS];
static uint32_t generation;
static struct aster_net_stats counters;
static struct k_thread worker_thread;
static k_thread_stack_t *worker_stack;
K_MUTEX_DEFINE(net_lock);
K_SEM_DEFINE(net_wake, 0, 1);

static size_t buffer_size(enum aster_net_kind kind)
{
    return kind == ASTER_NET_TCP ? sizeof(((union buffers *)0)->tcp) :
                                  sizeof(((union buffers *)0)->udp);
}
static struct slot *lookup(uint64_t owner, uint32_t handle)
{
    for (int i = 0; i < ASTER_NET_SLOTS; ++i)
        if (slots[i].handle == handle && handle && slots[i].owner == owner &&
            !slots[i].closing) return &slots[i];
    return NULL;
}
static bool terminal(const struct slot *s)
{
    return s->state == ASTER_NET_FAILED || s->state == ASTER_NET_CANCELLED;
}
static int socket_error(void) { return errno ? -errno : -EIO; }
static void fail(struct slot *s, int error)
{
    if (terminal(s)) return;
    s->error = error;
    if (error == -ECANCELED) s->phase = ASTER_NET_CANCEL;
    s->state = error == -ECANCELED ? ASTER_NET_CANCELLED : ASTER_NET_FAILED;
    s->tx_len = 0;
    if (error == -ECANCELED) ++counters.cancelled;
    else ++counters.failed;
}
/* Callback may run synchronously inside dns_get_addr_info/cancel. These calls
 * are made WITHOUT net_lock; numeric generation token outlives all contexts. */
static void resolved(enum dns_resolve_status status, struct dns_addrinfo *info,
                     void *token)
{
    uint32_t handle = (uint32_t)(uintptr_t)token;
    k_mutex_lock(&net_lock, K_FOREVER);
    for (int i = 0; i < ASTER_NET_SLOTS; ++i) {
        struct slot *s = &slots[i];
        if (s->handle != handle || !s->dns_started) continue;
        if (status == DNS_EAI_INPROGRESS && info && info->ai_family == AF_INET &&
            !s->addr_ready) {
            s->ip = ((struct sockaddr_in *)&info->ai_addr)->sin_addr.s_addr;
            s->addr_ready = true;
        } else if (status != DNS_EAI_INPROGRESS) {
            s->dns_done = true;
        }
        break;
    }
    k_mutex_unlock(&net_lock);
    k_sem_give(&net_wake);
}
/* Called only by the worker. Dropping net_lock cannot recycle the slot:
 * callers only mark close/cancel; the sole worker reclaims it later. */
static void stop_dns(struct slot *s)
{
    if (!s->dns_started) return;
    uint16_t id = s->dns_id;
    k_mutex_unlock(&net_lock);
    (void)dns_cancel_addr_info_with_name(s->host, DNS_QUERY_TYPE_A, id);
    k_mutex_lock(&net_lock, K_FOREVER);
    s->dns_started = false;
}
static bool unicast(uint32_t ip)
{
    uint32_t first = ntohl(ip) >> 24;
    return first != 0 && first < 224;
}
static int address(struct slot *s, int64_t now)
{
    if (s->addr_ready && (!s->dns_started || s->dns_done)) {
        stop_dns(s);
        return unicast(s->ip) ? 1 : -EAFNOSUPPORT;
    }
    if (s->dns_started) {
        if (s->dns_done) { stop_dns(s); return -EDESTADDRREQ; }
        return 0;
    }
    struct in_addr ip;
    if (zsock_inet_pton(AF_INET, s->host, &ip) == 1) {
        s->ip = ip.s_addr;
        if (!unicast(s->ip)) return -EAFNOSUPPORT;
        s->addr_ready = true;
        return 1;
    }
    s->phase = ASTER_NET_DNS;
    s->dns_started = true;
    s->dns_done = false;
    s->state = ASTER_NET_RESOLVING;
    int32_t timeout = (int32_t)(s->deadline - now);
    k_mutex_unlock(&net_lock);
    int rc = dns_get_addr_info(s->host, DNS_QUERY_TYPE_A, &s->dns_id,
                              resolved, (void *)(uintptr_t)s->handle, timeout);
    k_mutex_lock(&net_lock, K_FOREVER);
    if (rc < 0) { s->dns_started = false;
        return rc == -ENOMEM || rc == -EAGAIN ? rc : -EDESTADDRREQ; }
    return 0;
}
static struct sockaddr_in peer(const struct slot *s)
{
    return (struct sockaddr_in){ .sin_family = AF_INET,
        .sin_port = htons(s->port), .sin_addr.s_addr = s->ip };
}
static void local_port(struct slot *s)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    if (!zsock_getsockname(s->fd, (struct sockaddr *)&a, &n))
        s->local_port = ntohs(a.sin_port);
}
static int make_socket(struct slot *s)
{
    s->phase = ASTER_NET_SOCKET;
    s->fd = zsock_socket(AF_INET, s->kind == ASTER_NET_TCP ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (s->fd < 0) return socket_error();
    if (zsock_fcntl(s->fd, F_SETFL, O_NONBLOCK) < 0) return socket_error();
    if (s->kind == ASTER_NET_UDP) {
        struct sockaddr_in a = { .sin_family = AF_INET };
        if (zsock_bind(s->fd, (struct sockaddr *)&a, sizeof(a)) < 0) return socket_error();
        local_port(s);
    }
    return 0;
}
static void tcp_io(struct slot *s, int64_t now)
{
    if (s->state == ASTER_NET_OPENING || s->state == ASTER_NET_RESOLVING) {
        if (now >= s->deadline) { fail(s, -ETIMEDOUT); return; }
        int rc = address(s, now);
        if (rc < 0) { fail(s, rc); return; }
        if (!rc || s->closing || s->cancel) return;
        struct sockaddr_in a = peer(s);
        s->phase = ASTER_NET_CONNECT;
        rc = zsock_connect(s->fd, (struct sockaddr *)&a, sizeof(a));
        if (rc < 0 && errno != EINPROGRESS && errno != EALREADY) {
            fail(s, socket_error()); return;
        }
        s->state = rc == 0 ? ASTER_NET_READY : ASTER_NET_CONNECTING;
    }
    if (s->state == ASTER_NET_CONNECTING) {
        if (now >= s->deadline) { fail(s, -ETIMEDOUT); return; }
        struct zsock_pollfd p = { .fd = s->fd, .events = ZSOCK_POLLOUT };
        int rc = zsock_poll(&p, 1, 0);
        if (rc < 0) { fail(s, socket_error()); return; }
        if (!rc) return;
        int error = 0;
        socklen_t n = sizeof(error);
        if (zsock_getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &n) < 0) {
            fail(s, socket_error()); return;
        }
        if (error) { fail(s, -error); return; }
        s->state = ASTER_NET_READY;
    }
    local_port(s);
    if (s->tx_len) {
        s->phase = ASTER_NET_TX;
        int n = zsock_send(s->fd, s->buf->tcp.tx, s->tx_len, MSG_DONTWAIT);
        if (n > 0) {
            s->tx_len -= n;
            memmove(s->buf->tcp.tx, s->buf->tcp.tx + n, s->tx_len);
            s->tx_bytes += n;
            s->tx_deadline = now + IO_TIMEOUT;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            fail(s, socket_error()); return;
        } else if (now >= s->tx_deadline) { fail(s, -ETIMEDOUT); return; }
    }
    if (s->state == ASTER_NET_EOF || s->rx_len == ASTER_NET_TCP_BUFFER) return;
    s->phase = ASTER_NET_RX;
    int n = zsock_recv(s->fd, s->buf->tcp.rx + s->rx_len,
                       ASTER_NET_TCP_BUFFER - s->rx_len, MSG_DONTWAIT);
    if (n > 0) { s->rx_len += n; s->rx_bytes += n; }
    else if (!n) {
        if (s->tx_len) fail(s, -EPIPE);
        else s->state = ASTER_NET_EOF;
    }
    else if (errno != EAGAIN && errno != EWOULDBLOCK) fail(s, socket_error());
}
static void udp_io(struct slot *s, int64_t now)
{
    if (s->state == ASTER_NET_OPENING) s->state = ASTER_NET_READY;
    if (s->tx_len) {
        if (now >= s->deadline) { fail(s, -ETIMEDOUT); return; }
        int rc = address(s, now);
        if (rc < 0) { fail(s, rc); return; }
        if (rc && !s->closing && !s->cancel) {
            struct outbound *q = &s->buf->udp.tx[0];
            struct datagram *d = &q->packet;
            struct sockaddr_in a = peer(s);
            s->phase = ASTER_NET_TX;
            int n = zsock_sendto(s->fd, d->data, d->length, MSG_DONTWAIT,
                                 (struct sockaddr *)&a, sizeof(a));
            if (n == d->length) {
                s->tx_bytes += n;
                --s->tx_len;
                memmove(q, q + 1, s->tx_len * sizeof(*q));
                s->addr_ready = false;
                s->state = ASTER_NET_READY;
                if (s->tx_len) {
                    strcpy(s->host, q->host); s->port = q->packet.port;
                    s->deadline = q->deadline;
                }
            } else if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                fail(s, n >= 0 ? -EIO : socket_error()); return;
            }
        }
    }
    /* Scratch lives in PSRAM, not on the 4 KiB worker stack. Read even when
     * full so drops are counted. MSG_TRUNC reports over-limit packet length. */
    struct datagram *d = &s->buf->udp.rx[ASTER_NET_UDP_DEPTH];
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    s->phase = ASTER_NET_RX;
    int n = zsock_recvfrom(s->fd, d->data, ASTER_NET_UDP_MAX, MSG_DONTWAIT | MSG_TRUNC,
                           (struct sockaddr *)&a, &len);
    if (n >= 0) {
        if (n > ASTER_NET_UDP_MAX || s->rx_len == ASTER_NET_UDP_DEPTH) {
            ++s->dropped; ++counters.dropped;
        } else {
            d->length = n; d->ip = a.sin_addr.s_addr; d->port = ntohs(a.sin_port);
            s->buf->udp.rx[s->rx_len++] = *d;
            s->rx_bytes += n;
        }
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) fail(s, socket_error());
}
static bool service_tick(void)
{
    k_mutex_lock(&net_lock, K_FOREVER);
    for (int i = 0; i < ASTER_NET_SLOTS; ++i) {
        struct slot *s = &slots[i];
        if (!s->handle) continue;
        if (s->restarting) {
            int fd = s->fd;
            s->fd = -1;
            if (fd >= 0) {
                k_mutex_unlock(&net_lock);
                zsock_close(fd);
                k_mutex_lock(&net_lock, K_FOREVER);
            }
            s->restarting = false;
        }
        if (s->cancel) fail(s, -ECANCELED);
        if (!s->closing && !s->retired && !terminal(s)) {
            int rc = s->fd < 0 ? make_socket(s) : 0;
            if (rc) fail(s, rc);
            else if (s->kind == ASTER_NET_TCP) tcp_io(s, k_uptime_get());
            else udp_io(s, k_uptime_get());
        }
        if (s->closing || s->retired || terminal(s)) {
            stop_dns(s);
            if (s->fd >= 0) {
                /* close may wait for TCP protocol teardown; never hold the
                 * service lock while waiting. Slot stays charged until done. */
                int fd = s->fd; s->fd = -1;
                k_mutex_unlock(&net_lock);
                zsock_close(fd);
                k_mutex_lock(&net_lock, K_FOREVER);
            }
        }
        if (s->closing) {
            counters.psram_bytes -= buffer_size(s->kind);
            aster_psram_free(s->buf);
            memset(s, 0, sizeof(*s));
            ++counters.closed;
        }
    }
    bool active = false;
    for (int i = 0; i < ASTER_NET_SLOTS; ++i) active |= slots[i].handle != 0;
    k_mutex_unlock(&net_lock);
    return active;
}
static void worker(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    for (;;) { bool active = service_tick(); k_sem_take(&net_wake, active ? K_MSEC(10) : K_FOREVER); }
}
int aster_net_open(uint64_t owner, enum aster_net_kind kind,
                   const char *host, uint16_t port, uint32_t timeout_ms)
{
    if (!owner || (kind != ASTER_NET_TCP && kind != ASTER_NET_UDP) ||
        !timeout_ms || timeout_ms > 30000 ||
        (kind == ASTER_NET_TCP && (!host || !host[0] ||
         strlen(host) > ASTER_NET_HOST_MAX || !port))) return -EINVAL;
    if (k_mutex_lock(&net_lock, K_NO_WAIT)) return -EAGAIN;
    struct slot *s = NULL;
    for (int i = 0; i < ASTER_NET_SLOTS; ++i)
        if (!slots[i].handle) { s = &slots[i]; break; }
    if (!s) { k_mutex_unlock(&net_lock); return -ENOSPC; }
    if (generation == INT32_MAX) { k_mutex_unlock(&net_lock); return -EOVERFLOW; }
    union buffers *buf = aster_psram_alloc(buffer_size(kind));
    if (!buf) { k_mutex_unlock(&net_lock); return -ENOMEM; }
    if (!worker_stack) {
        worker_stack = aster_dram_alloc(K_KERNEL_STACK_LEN(NET_STACK));
        if (!worker_stack) {
            aster_psram_free(buf); k_mutex_unlock(&net_lock); return -ENOMEM;
        }
        k_thread_create(&worker_thread, worker_stack, NET_STACK, worker,
                        NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
        k_thread_name_set(&worker_thread, "aster_net");
        counters.stack_bytes = K_KERNEL_STACK_LEN(NET_STACK);
    }
    memset(s, 0, sizeof(*s));
    s->owner = owner; s->handle = ++generation; s->kind = kind;
    s->state = ASTER_NET_OPENING; s->phase = ASTER_NET_SOCKET; s->fd = -1; s->buf = buf;
    s->port = port; s->deadline = k_uptime_get() + timeout_ms;
    if (kind == ASTER_NET_TCP) strcpy(s->host, host);
    int handle = s->handle;
    ++counters.opened; counters.psram_bytes += buffer_size(kind);
    k_mutex_unlock(&net_lock); k_sem_give(&net_wake);
    return handle;
}
/* Internal HTTP redirect: preserve admission and buffers while replacing the
 * completed DNS/connected transport. Generation changes before returning, so
 * stale users/callbacks cannot address the next connection. */
int aster_net_restart(uint64_t owner, uint32_t handle, const char *host,
                      uint16_t port, uint32_t timeout_ms)
{
    if (!host || !host[0] || strlen(host) > ASTER_NET_HOST_MAX || !port ||
        !timeout_ms || timeout_ms > 30000) return -EINVAL;
    if (k_mutex_lock(&net_lock, K_NO_WAIT)) return -EAGAIN;
    struct slot *s = lookup(owner, handle);
    int rc = -EBADF;
    if (!s) goto out;
    if (s->kind != ASTER_NET_TCP || s->dns_started || s->restarting ||
        (s->state != ASTER_NET_READY && s->state != ASTER_NET_EOF)) {
        rc = -EBUSY; goto out;
    }
    if (s->cancel) { rc = -ECANCELED; goto out; }
    if (generation == INT32_MAX) { rc = -EOVERFLOW; goto out; }
    uint64_t saved_owner = s->owner;
    union buffers *saved_buf = s->buf;
    int saved_fd = s->fd;
    memset(s, 0, sizeof(*s));
    s->owner = saved_owner; s->buf = saved_buf; s->fd = saved_fd;
    s->handle = ++generation; s->kind = ASTER_NET_TCP;
    s->state = ASTER_NET_OPENING; s->phase = ASTER_NET_SOCKET;
    s->restarting = true; s->port = port;
    s->deadline = k_uptime_get() + timeout_ms;
    strcpy(s->host, host);
    ++counters.closed; ++counters.opened;
    rc = s->handle;
out:
    k_mutex_unlock(&net_lock); k_sem_give(&net_wake); return rc;
}
/* Successful framed HTTP responses can finish before a TCP FIN. Retire the
 * fd without advertising cancellation or releasing the reserved slot. */
int aster_net_finish(uint64_t owner, uint32_t handle)
{
    if (k_mutex_lock(&net_lock, K_NO_WAIT)) return -EAGAIN;
    struct slot *s = lookup(owner, handle);
    int rc = -EBADF;
    if (s) {
        if (s->kind != ASTER_NET_TCP || s->dns_started) rc = -EINVAL;
        else {
            s->retired = true;
            s->tx_len = s->rx_len = 0;
            if (!terminal(s)) s->state = ASTER_NET_EOF;
            rc = 0;
        }
    }
    k_mutex_unlock(&net_lock); k_sem_give(&net_wake); return rc;
}
int aster_net_snapshot(uint64_t owner, uint32_t handle, struct aster_net_snapshot *out)
{
    if (!out) return -EINVAL;
    if (k_mutex_lock(&net_lock, K_NO_WAIT)) return -EAGAIN;
    struct slot *s = lookup(owner, handle);
    if (s) *out = (struct aster_net_snapshot){
        .handle = s->handle, .kind = s->kind, .state = s->state, .phase = s->phase, .error = s->error,
        .tx_queued = s->tx_len, .rx_queued = s->rx_len, .dropped = s->dropped,
        .tx_bytes = s->tx_bytes, .rx_bytes = s->rx_bytes, .local_port = s->local_port };
    k_mutex_unlock(&net_lock);
    return s ? 0 : -EBADF;
}
int aster_net_send(uint64_t owner, uint32_t handle, const void *data,
                   size_t length, const char *host, uint16_t port)
{
    if (!data && length) return -EINVAL;
    if (k_mutex_lock(&net_lock, K_NO_WAIT)) return -EAGAIN;
    struct slot *s = lookup(owner, handle);
    int rc = -EBADF;
    if (!s) goto out;
    if (s->cancel) { rc = -ECANCELED; goto out; }
    if (terminal(s)) { rc = s->error; goto out; }
    if (s->state == ASTER_NET_EOF) { rc = -EPIPE; goto out; }
    if (s->state != ASTER_NET_READY) {
        rc = -EAGAIN; goto out;
    }
    if (s->kind == ASTER_NET_TCP) {
        size_t n = ASTER_NET_TCP_BUFFER - s->tx_len;
        if (n > length) n = length;
        rc = length && !n ? -EAGAIN : (int)n;
        if (!s->tx_len && n) s->tx_deadline = k_uptime_get() + IO_TIMEOUT;
        if (n) memcpy(s->buf->tcp.tx + s->tx_len, data, n);
        s->tx_len += n;
    } else {
        if (!host || !host[0] || strlen(host) > ASTER_NET_HOST_MAX || !port) {
            rc = -EINVAL; goto out;
        }
        if (length > ASTER_NET_UDP_MAX) { rc = -EMSGSIZE; goto out; }
        if (s->tx_len == ASTER_NET_UDP_DEPTH) { rc = -EAGAIN; goto out; }
        struct outbound *q = &s->buf->udp.tx[s->tx_len];
        struct datagram *d = &q->packet;
        if (length) memcpy(d->data, data, length);
        d->length = length; strcpy(q->host, host); d->port = port;
        q->deadline = k_uptime_get() + IO_TIMEOUT;
        if (!s->tx_len) {
            strcpy(s->host, host); s->port = port;
            s->deadline = k_uptime_get() + IO_TIMEOUT;
        }
        ++s->tx_len; rc = length;
    }
out:
    k_mutex_unlock(&net_lock); k_sem_give(&net_wake); return rc;
}
int aster_net_recv(uint64_t owner, uint32_t handle, void *data, size_t capacity,
                   size_t *required, uint32_t *ipv4, uint16_t *port)
{
    if ((!data && capacity) || !required) return -EINVAL;
    *required = 0;
    if (k_mutex_lock(&net_lock, K_NO_WAIT)) return -EAGAIN;
    struct slot *s = lookup(owner, handle);
    int rc = -EBADF;
    if (!s) goto out;
    if (s->kind == ASTER_NET_TCP) {
        if (!capacity) { rc = -EINVAL; goto out; }
        if (s->rx_len) {
            size_t n = s->rx_len < capacity ? s->rx_len : capacity;
            memcpy(data, s->buf->tcp.rx, n); s->rx_len -= n;
            memmove(s->buf->tcp.rx, s->buf->tcp.rx + n, s->rx_len);
            *required = n; rc = n;
        } else rc = terminal(s) ? s->error : s->cancel ? -ECANCELED :
                         s->state == ASTER_NET_EOF ? 0 : -EAGAIN;
    } else if (s->rx_len) {
        struct datagram *d = &s->buf->udp.rx[0];
        *required = d->length;
        if (capacity < d->length) { rc = -EMSGSIZE; goto out; }
        if (d->length) memcpy(data, d->data, d->length);
        if (ipv4) *ipv4 = d->ip;
        if (port) *port = d->port;
        rc = d->length; --s->rx_len;
        memmove(d, d + 1, s->rx_len * sizeof(*d));
    } else rc = terminal(s) ? s->error : s->cancel ? -ECANCELED : -EAGAIN;
out:
    k_mutex_unlock(&net_lock); k_sem_give(&net_wake); return rc;
}
static int end(uint64_t owner, uint32_t handle, bool close)
{
    if (k_mutex_lock(&net_lock, K_NO_WAIT)) return -EAGAIN;
    struct slot *s = lookup(owner, handle);
    if (s) { if (close) s->closing = true; else s->cancel = true; }
    k_mutex_unlock(&net_lock); k_sem_give(&net_wake);
    return s ? 0 : -EBADF;
}
int aster_net_cancel(uint64_t o, uint32_t h) { return end(o, h, false); }
int aster_net_close(uint64_t o, uint32_t h) { return end(o, h, true); }
int aster_net_close_owner(uint64_t owner)
{
    if (k_mutex_lock(&net_lock, K_NO_WAIT)) return -EAGAIN;
    for (int i = 0; i < ASTER_NET_SLOTS; ++i)
        if (slots[i].handle && slots[i].owner == owner) slots[i].closing = true;
    k_mutex_unlock(&net_lock); k_sem_give(&net_wake);
    return 0;
}
int aster_net_get_stats(struct aster_net_stats *out)
{
    if (!out) return -EINVAL;
    if (k_mutex_lock(&net_lock, K_NO_WAIT)) return -EAGAIN;
    *out = counters; out->active = out->closing = 0;
    for (int i = 0; i < ASTER_NET_SLOTS; ++i) {
        if (slots[i].handle) ++out->active;
        if (slots[i].closing) ++out->closing;
    }
    k_mutex_unlock(&net_lock);
    return 0;
}

/* OS metadata getters, no radio control, DNS or socket wait. Fields are a
 * best-effort snapshot: callers must not infer internet access from flags. */
int aster_net_interface_info(struct aster_net_interface_info *out)
{
    if (!out) return -EINVAL;
    *out = (struct aster_net_interface_info){0};
    struct net_if *iface = net_if_get_default();
    if (!iface) return 0;
    if (k_mutex_lock(&iface->lock, K_NO_WAIT)) return -EAGAIN;
    out->flags = 1u;
    if (net_if_is_up(iface)) out->flags |= 2u;
    if (net_if_is_carrier_ok(iface)) out->flags |= 4u;
    const struct in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (addr) { out->ipv4 = addr->s_addr; out->flags |= 8u; }
    k_mutex_unlock(&iface->lock);
    return 0;
}
