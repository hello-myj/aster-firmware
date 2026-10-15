/* SPDX-License-Identifier: MIT */
#include "aster_http.h"
#include "aster_mem.h"
#include "aster_net.h"
#if defined(CONFIG_ASTER_HTTP_TLS)
#include "aster_http_tls.h"
#endif
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifndef ASTER_HTTP_HOST_TEST
#include <zephyr/kernel.h>
#endif
#define HTTP_STACK 8192
#define HTTP_IO 1024
#define HTTP_TX_MAX (ASTER_HTTP_URL_MAX + ASTER_HTTP_HEADERS_MAX + 512)
struct http_request {
    uint64_t owner, transport_owner;
    uint32_t transport;
    bool closing, cancel, stop_pending, redirect_pending, tls_reserved;
#if defined(CONFIG_ASTER_HTTP_TLS)
    struct aster_http_tls *tls;
    uint32_t tls_bytes;
    struct aster_http_tls_report tls_report;
#endif
    struct aster_http_snapshot pub;
    struct aster_http_headers published_headers;
    uint8_t body[ASTER_HTTP_BODY_BUFFER];
    struct aster_http_options opts;
    struct aster_http_url url, next_url;
    char custom[ASTER_HTTP_HEADERS_MAX + 1], tx[HTTP_TX_MAX];
    uint8_t upload[ASTER_HTTP_UPLOAD_MAX], input[HTTP_IO], decoded[HTTP_IO];
    size_t tx_length, tx_offset, upload_offset, input_length, input_offset;
    uint32_t redirects;
    int64_t deadline;
    struct aster_http_codec codec;
};
static struct http_request *requests[ASTER_NET_SLOTS];
static struct k_thread http_thread;
static k_thread_stack_t *http_stack;
static uint64_t http_generation;
K_MUTEX_DEFINE(http_lock);
K_SEM_DEFINE(http_wake, 0, 1);
#if defined(CONFIG_ASTER_HTTP_TLS)
static char *custom_ca;
static size_t custom_ca_length;
static bool tls_busy(void)
{
    for (unsigned i = 0; i < ASTER_NET_SLOTS; ++i)
        if (requests[i] && requests[i]->tls_reserved)
            return true;
    return false;
}
#endif
#if defined(CONFIG_ASTER_HTTP_TLS)
static int tls_release(struct http_request *r)
{
    int rc = aster_http_tls_free(&r->tls);
    if (!rc) {
        k_mutex_lock(&http_lock, K_FOREVER);
        r->tls_bytes = 0;
        k_mutex_unlock(&http_lock);
    }
    return rc;
}
#endif
static bool finished(uint32_t state) { return state >= ASTER_HTTP_DONE; }
static struct http_request *find(uint64_t owner, uint32_t handle)
{
    for (unsigned i = 0; i < ASTER_NET_SLOTS; ++i)
        if (requests[i] && requests[i]->owner == owner &&
            requests[i]->pub.handle == handle && !requests[i]->closing)
            return requests[i];
    return NULL;
}
static void state(struct http_request *r, uint32_t value, int error)
{
    k_mutex_lock(&http_lock, K_FOREVER);
    r->pub.state = value;
    r->pub.error = error;
#if defined(CONFIG_ASTER_HTTP_TLS)
    r->pub.tls_error = r->tls_report.error;
    r->pub.transport_error = r->tls_report.transport_error;
    r->pub.tls_verify = r->tls_report.verify;
#endif
    if (finished(value))
        r->stop_pending = true;
    k_mutex_unlock(&http_lock);
}
static int request_head(struct http_request *r)
{
    const char *method = r->opts.method == ASTER_HTTP_HEAD   ? "HEAD"
                         : r->opts.method == ASTER_HTTP_POST ? "POST"
                                                             : "GET";
    int n = snprintf(r->tx, sizeof(r->tx),
                     "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: "
                     "close\r\nAccept-Encoding: "
                     "identity\r\n%sContent-Length: %u\r\n\r\n",
                     method, r->url.path, r->url.host, r->url.port, r->custom,
                     (unsigned)r->opts.body_length);
    if (n < 0 || (size_t)n >= sizeof(r->tx))
        return -EMSGSIZE;
    r->tx_length = n;
    r->tx_offset = r->upload_offset = r->input_length = r->input_offset = 0;
    aster_http_codec_init(&r->codec, r->opts.method == ASTER_HTTP_HEAD,
                          r->opts.body_limit);
    return 0;
}
static int redirect_prepare(struct http_request *r)
{
    unsigned code = r->codec.status;
    if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308)
        return 0;
    size_t n;
    int rc = aster_http_field_get(&r->codec.headers, "Location", 0, r->tx,
                                  sizeof(r->tx) - 1, &n);
    if (rc == -ENOENT)
        return 0;
    if (rc)
        return rc;
    if (!n)
        return -EPROTO;
    r->tx[n] = 0;
    if (r->redirects == ASTER_HTTP_REDIRECT_MAX)
        return -ELOOP;
    rc = aster_http_redirect(&r->url, r->tx, &r->next_url);
    if (rc)
        return rc;
    if (r->url.tls && !r->next_url.tls)
        return -EACCES;
    if (r->next_url.tls && !r->tls_reserved) {
#if defined(CONFIG_ASTER_HTTP_TLS)
        k_mutex_lock(&http_lock, K_FOREVER);
        bool busy = tls_busy();
        if (!busy)
            r->tls_reserved = true;
        k_mutex_unlock(&http_lock);
        if (busy)
            return -EBUSY;
#else
        return -ENOTSUP;
#endif
    }
    if (!aster_http_same_origin(&r->url, &r->next_url))
        r->custom[0] = 0;
    if ((code == 303 && r->opts.method != ASTER_HTTP_HEAD) ||
        ((code == 301 || code == 302) && r->opts.method == ASTER_HTTP_POST)) {
        r->opts.method = ASTER_HTTP_GET;
        r->opts.body_length = 0;
    }
    r->redirect_pending = true;
    return 1;
}
static void process(struct http_request *r, bool cancelled)
{
    if (cancelled && !finished(r->pub.state))
        state(r, ASTER_HTTP_CANCELLED, -ECANCELED);
    if (!finished(r->pub.state) && k_uptime_get() >= r->deadline)
        state(r, ASTER_HTTP_FAILED, -ETIMEDOUT);
    if (finished(r->pub.state)) {
#if defined(CONFIG_ASTER_HTTP_TLS)
        if (tls_release(r) == -EAGAIN)
            return;
#endif
        if (r->stop_pending) {
            int rc = r->pub.state == ASTER_HTTP_DONE
                         ? aster_net_finish(r->transport_owner, r->transport)
                         : aster_net_cancel(r->transport_owner, r->transport);
            if (rc != -EAGAIN)
                r->stop_pending = false;
        }
        return;
    }
    if (r->redirect_pending) {
#if defined(CONFIG_ASTER_HTTP_TLS)
        if (tls_release(r) == -EAGAIN)
            return;
#endif
        int64_t remaining = r->deadline - k_uptime_get();
        int rc = aster_net_restart(r->transport_owner, r->transport,
                                   r->next_url.host, r->next_url.port,
                                   remaining > 30000 ? 30000 : remaining);
        if (rc == -EAGAIN)
            return;
        if (rc < 0) {
            state(r, ASTER_HTTP_FAILED, rc);
            return;
        }
        r->transport = rc;
        r->url = r->next_url;
        r->redirect_pending = false;
        ++r->redirects;
        rc = request_head(r);
        if (rc) {
            state(r, ASTER_HTTP_FAILED, rc);
            return;
        }
        k_mutex_lock(&http_lock, K_FOREVER);
        r->pub.redirects = r->redirects;
        r->pub.tls = r->url.tls;
        k_mutex_unlock(&http_lock);
        state(r, ASTER_HTTP_CONNECTING, 0);
    }
    if (r->pub.state == ASTER_HTTP_CONNECTING) {
        struct aster_net_snapshot s;
        int rc = aster_net_snapshot(r->transport_owner, r->transport, &s);
        if (rc == -EAGAIN)
            return;
        if (rc < 0) {
            state(r, ASTER_HTTP_FAILED, rc);
            return;
        }
        if (s.state == ASTER_NET_FAILED || s.state == ASTER_NET_CANCELLED) {
            state(r, ASTER_HTTP_FAILED, s.error);
            return;
        }
        if (s.state != ASTER_NET_READY && s.state != ASTER_NET_EOF)
            return;
        state(r, r->url.tls ? ASTER_HTTP_HANDSHAKE : ASTER_HTTP_SENDING, 0);
    }
#if defined(CONFIG_ASTER_HTTP_TLS)
    if (r->pub.state == ASTER_HTTP_HANDSHAKE) {
        int rc = 0;
        if (!r->tls) {
            size_t length;
            const char *pem;
            if (custom_ca) {
                pem = custom_ca;
                length = custom_ca_length;
            } else
                pem = aster_http_tls_default_ca(&length);
            rc = aster_http_tls_new(&r->tls, r->transport_owner, r->transport,
                                    r->url.host, pem, length, &r->tls_report);
            if (!rc) {
                k_mutex_lock(&http_lock, K_FOREVER);
                r->tls_bytes = aster_http_tls_context_bytes();
                k_mutex_unlock(&http_lock);
            }
        }
        if (!rc)
            rc = aster_http_tls_handshake(r->tls, &r->tls_report);
        if (rc == -EAGAIN)
            return;
        if (rc) {
            state(r, ASTER_HTTP_FAILED, rc);
            return;
        }
        state(r, ASTER_HTTP_SENDING, 0);
    }
#endif
    int tx_error = 0;
    if (r->pub.state == ASTER_HTTP_SENDING) {
        const uint8_t *p;
        size_t n;
        if (r->tx_offset < r->tx_length) {
            p = (void *)(r->tx + r->tx_offset);
            n = r->tx_length - r->tx_offset;
        } else {
            p = r->upload + r->upload_offset;
            n = r->opts.body_length - r->upload_offset;
        }
        if (n) {
            if (n > HTTP_IO)
                n = HTTP_IO;
            int rc;
#if defined(CONFIG_ASTER_HTTP_TLS)
            if (r->tls)
                rc = aster_http_tls_send(r->tls, p, n, &r->tls_report);
            else
#endif
                rc = aster_net_send(r->transport_owner, r->transport, p, n,
                                    NULL, 0);
            if (rc < 0 && rc != -EAGAIN)
                tx_error = rc;
            if (rc > 0) {
                if (r->tx_offset < r->tx_length)
                    r->tx_offset += rc;
                else
                    r->upload_offset += rc;
            }
            /* Receive while uploading: a final 4xx/redirect can arrive before
             * the body is sent. Interim 1xx keeps the upload in progress. */
        } else {
            state(r, ASTER_HTTP_HEADERS, 0);
        }
    }
    k_mutex_lock(&http_lock, K_FOREVER);
    size_t cap = sizeof(r->body) - r->pub.queued;
    k_mutex_unlock(&http_lock);
    if (!cap)
        return;
    if (cap > sizeof(r->decoded))
        cap = sizeof(r->decoded);
    if (r->input_offset == r->input_length) {
        size_t required;
        int rc;
#if defined(CONFIG_ASTER_HTTP_TLS)
        if (r->tls)
            rc = aster_http_tls_recv(r->tls, r->input, sizeof(r->input),
                                     &r->tls_report);
        else
#endif
            rc = aster_net_recv(r->transport_owner, r->transport, r->input,
                                sizeof(r->input), &required, NULL, NULL);
        if (rc == -EAGAIN) {
            if (tx_error)
                state(r, ASTER_HTTP_FAILED, tx_error);
            return;
        }
        if (rc < 0) {
            state(r, ASTER_HTTP_FAILED, rc);
            return;
        }
        if (!rc) {
            rc = aster_http_codec_eof(&r->codec);
            state(r, rc ? ASTER_HTTP_FAILED : ASTER_HTTP_DONE, rc);
            return;
        }
        r->input_offset = 0;
        r->input_length = rc;
    }
    bool previous = r->codec.headers_ready;
    size_t used, wrote;
    int rc = aster_http_codec_feed(&r->codec, r->input + r->input_offset,
                                   r->input_length - r->input_offset, &used,
                                   r->decoded, cap, &wrote);
    r->input_offset += used;
    if (!previous && r->codec.headers_ready) {
        int redirected = redirect_prepare(r);
        if (redirected > 0)
            return;
        if (redirected < 0) {
            state(r, ASTER_HTTP_FAILED, redirected);
            return;
        }
    }
    k_mutex_lock(&http_lock, K_FOREVER);
    if (r->codec.headers_ready) {
        r->published_headers = r->codec.headers;
        r->pub.headers_ready = true;
        r->pub.status = r->codec.status;
        r->pub.has_length = r->codec.has_length;
        r->pub.content_length = r->codec.content_length;
        r->pub.state = ASTER_HTTP_BODY;
    }
    memcpy(r->body + r->pub.queued, r->decoded, wrote);
    r->pub.queued += wrote;
    r->pub.received += wrote;
    k_mutex_unlock(&http_lock);
    if (rc || r->codec.state == HC_DONE)
        state(r, rc ? ASTER_HTTP_FAILED : ASTER_HTTP_DONE, rc);
}
static bool http_tick(void)
{
    bool active = false;
    for (unsigned i = 0; i < ASTER_NET_SLOTS; ++i) {
        k_mutex_lock(&http_lock, K_FOREVER);
        struct http_request *r = requests[i];
        bool closing = r && r->closing, cancelled = r && r->cancel;
        k_mutex_unlock(&http_lock);
        if (!r)
            continue;
        active = true;
        if (closing) {
#if defined(CONFIG_ASTER_HTTP_TLS)
            if (tls_release(r) == -EAGAIN)
                continue;
#endif
            if (aster_net_close(r->transport_owner, r->transport) == -EAGAIN)
                continue;
            k_mutex_lock(&http_lock, K_FOREVER);
            requests[i] = NULL;
            k_mutex_unlock(&http_lock);
            aster_psram_free(r);
            continue;
        }
        process(r, cancelled);
    }
    return active;
}
#ifndef ASTER_HTTP_HOST_TEST
/* A full-rate APP can keep priority-7 drawing runnable continuously. A lower
 * priority TLS worker can then starve inside scalar multiplication. Share that
 * priority, but force a 1 ms sleep after each 2 ms CPU slice so native TCP and
 * the shell retain scheduling opportunities. No blocking work runs in ISR. */
static atomic_t http_cpu_yields;
static void http_budget_resume(struct k_timer *timer)
{
    (void)timer;
    k_thread_resume(&http_thread);
}
K_TIMER_DEFINE(http_budget_timer, http_budget_resume, NULL);
static void http_budget_expired(struct k_thread *thread, void *data)
{
    (void)data;
    atomic_inc(&http_cpu_yields);
    k_thread_suspend(thread);
    k_timer_start(&http_budget_timer, K_MSEC(1), K_NO_WAIT);
}
#endif
static void http_worker(void *a, void *b, void *c)
{
    (void)a;
    (void)b;
    (void)c;
#ifndef ASTER_HTTP_HOST_TEST
    k_thread_time_slice_set(&http_thread, k_ms_to_ticks_ceil32(2),
                            http_budget_expired, NULL);
#endif
    for (;;) {
        bool active = http_tick();
        k_sem_take(&http_wake, active ? K_MSEC(5) : K_FOREVER);
    }
}
int aster_http_svc_start(uint64_t owner, const struct aster_http_options *o)
{
    if (!owner || owner > INT64_MAX || !o || o->method < ASTER_HTTP_GET ||
        o->method > ASTER_HTTP_POST || !o->timeout_ms ||
        o->timeout_ms > 600000 || !o->body_limit || o->body_limit > 67108864 ||
        o->body_length > ASTER_HTTP_UPLOAD_MAX ||
        (!o->body && o->body_length) ||
        (o->method != ASTER_HTTP_POST && o->body_length))
        return -EINVAL;
    int rc = aster_http_validate_headers(o->headers, o->headers_length);
    if (rc)
        return rc;
    struct aster_http_url url;
    rc = aster_http_url_parse(o->url, &url);
    if (rc)
        return rc;
#if !defined(CONFIG_ASTER_HTTP_TLS)
    if (url.tls)
        return -ENOTSUP;
#endif
    if (k_mutex_lock(&http_lock, K_NO_WAIT))
        return -EAGAIN;
#if defined(CONFIG_ASTER_HTTP_TLS)
    if (url.tls && tls_busy()) {
        k_mutex_unlock(&http_lock);
        return -EBUSY;
    }
#endif
    unsigned slot;
    for (slot = 0; slot < ASTER_NET_SLOTS && requests[slot]; ++slot) {
    }
    if (slot == ASTER_NET_SLOTS) {
        rc = -ENOSPC;
        goto out;
    }
    if (http_generation == INT64_MAX) {
        rc = -EOVERFLOW;
        goto out;
    }
    struct http_request *r = aster_psram_alloc(sizeof(*r));
    if (!r) {
        rc = -ENOMEM;
        goto out;
    }
    memset(r, 0, sizeof(*r));
    r->owner = owner;
    r->url = url;
    r->opts = *o;
    r->tls_reserved = url.tls;
    /* No externally owned pointer retained, including options.url/body. */
    r->opts.url = NULL;
    r->opts.headers = NULL;
    r->opts.body = NULL;
    if (o->headers_length)
        memcpy(r->custom, o->headers, o->headers_length);
    if (o->body_length)
        memcpy(r->upload, o->body, o->body_length);
    r->transport_owner = UINT64_C(0x8000000000000000) | ++http_generation;
    r->deadline = k_uptime_get() + o->timeout_ms;
    rc = request_head(r);
    if (rc) {
        aster_psram_free(r);
        goto out;
    }
    if (!http_stack) {
        http_stack = aster_dram_alloc(K_KERNEL_STACK_LEN(HTTP_STACK));
        if (!http_stack) {
            aster_psram_free(r);
            rc = -ENOMEM;
            goto out;
        }
        k_thread_create(&http_thread, http_stack, HTTP_STACK, http_worker, NULL,
                        NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
        k_thread_name_set(&http_thread, "aster_http");
    }
    rc = aster_net_open(r->transport_owner, ASTER_NET_TCP, url.host, url.port,
                        o->timeout_ms > 30000 ? 30000 : o->timeout_ms);
    if (rc < 0) {
        aster_psram_free(r);
        goto out;
    }
    r->transport = rc;
    r->pub.handle = rc;
    r->pub.state = ASTER_HTTP_CONNECTING;
    r->pub.tls = url.tls;
    requests[slot] = r;
out:
    k_mutex_unlock(&http_lock);
    k_sem_give(&http_wake);
    return rc;
}
int aster_http_svc_status(uint64_t owner, uint32_t h,
                      struct aster_http_snapshot *out)
{
    if (!out)
        return -EINVAL;
    if (k_mutex_lock(&http_lock, K_NO_WAIT))
        return -EAGAIN;
    struct http_request *r = find(owner, h);
    if (r)
        *out = r->pub;
    k_mutex_unlock(&http_lock);
    return r ? 0 : -EBADF;
}
int aster_http_svc_read(uint64_t owner, uint32_t h, void *out, size_t cap)
{
    if (!out || !cap)
        return -EINVAL;
    if (k_mutex_lock(&http_lock, K_NO_WAIT))
        return -EAGAIN;
    struct http_request *r = find(owner, h);
    int rc = -EBADF;
    if (r) {
        if (r->pub.queued) {
            size_t n = cap < r->pub.queued ? cap : r->pub.queued;
            memcpy(out, r->body, n);
            r->pub.queued -= n;
            memmove(r->body, r->body + n, r->pub.queued);
            rc = n;
        } else
            rc = finished(r->pub.state) ? r->pub.error : -EAGAIN;
    }
    k_mutex_unlock(&http_lock);
    k_sem_give(&http_wake);
    return rc;
}
int aster_http_svc_header(uint64_t owner, uint32_t h, const char *name,
                      unsigned index, char *out, size_t cap, size_t *length)
{
    if (k_mutex_lock(&http_lock, K_NO_WAIT))
        return -EAGAIN;
    struct http_request *r = find(owner, h);
    int rc = !r ? -EBADF
             : !r->pub.headers_ready
                 ? -EAGAIN
                 : aster_http_field_get(&r->published_headers, name, index, out,
                                        cap, length);
    k_mutex_unlock(&http_lock);
    return rc;
}
static int http_end(uint64_t owner, uint32_t h, bool close)
{
    if (k_mutex_lock(&http_lock, K_NO_WAIT))
        return -EAGAIN;
    struct http_request *r = find(owner, h);
    if (r) {
        if (close)
            r->closing = true;
        else
            r->cancel = true;
    }
    k_mutex_unlock(&http_lock);
    k_sem_give(&http_wake);
    return r ? 0 : -EBADF;
}
int aster_http_svc_close(uint64_t o, uint32_t h) { return http_end(o, h, true); }
int aster_http_svc_cancel(uint64_t o, uint32_t h) { return http_end(o, h, false); }
int aster_http_svc_close_owner(uint64_t owner)
{
    if (k_mutex_lock(&http_lock, K_NO_WAIT))
        return -EAGAIN;
    for (unsigned i = 0; i < ASTER_NET_SLOTS; ++i)
        if (requests[i] && requests[i]->owner == owner) {
            requests[i]->closing = true;
        }
    k_mutex_unlock(&http_lock);
    k_sem_give(&http_wake);
    /* Inputs are worker-owned copies. Accept teardown without waiting for
     * transport/crypto progress, just as the underlying network service does. */
    return 0;
}
int aster_http_svc_get_stats(struct aster_http_stats *out)
{
    if (!out)
        return -EINVAL;
    if (k_mutex_lock(&http_lock, K_NO_WAIT))
        return -EAGAIN;
    memset(out, 0, sizeof(*out));
    out->stack_bytes = http_stack ? K_KERNEL_STACK_LEN(HTTP_STACK) : 0;
#ifndef ASTER_HTTP_HOST_TEST
    out->cpu_yields = atomic_get(&http_cpu_yields);
#endif
#if defined(CONFIG_ASTER_HTTP_TLS)
    out->ca_bytes = custom_ca_length;
#endif
    for (unsigned i = 0; i < ASTER_NET_SLOTS; ++i)
        if (requests[i]) {
            ++out->active;
            out->tls_active += requests[i]->tls_reserved;
            out->closing += requests[i]->closing;
            out->psram_bytes += sizeof(struct http_request);
#if defined(CONFIG_ASTER_HTTP_TLS)
            out->psram_bytes += requests[i]->tls_bytes;
#endif
        }
    k_mutex_unlock(&http_lock);
    return 0;
}

int aster_http_svc_ca_set(const char *pem, size_t length)
{
#if defined(CONFIG_ASTER_HTTP_TLS)
    if ((!pem && length) || length > 8192 ||
        (pem && (!length || pem[length - 1] != 0 || strlen(pem) + 1 != length)))
        return -EINVAL;
    if (k_mutex_lock(&http_lock, K_NO_WAIT))
        return -EAGAIN;
    if (tls_busy()) {
        k_mutex_unlock(&http_lock);
        return -EBUSY;
    }
    char *copy = NULL;
    if (pem) {
        copy = aster_psram_alloc(length);
        if (!copy) {
            k_mutex_unlock(&http_lock);
            return -ENOMEM;
        }
        memcpy(copy, pem, length);
    }
    aster_psram_free(custom_ca);
    custom_ca = copy;
    custom_ca_length = length;
    k_mutex_unlock(&http_lock);
    return 0;
#else
    (void)pem;
    (void)length;
    return -ENOTSUP;
#endif
}
