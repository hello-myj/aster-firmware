/* SPDX-License-Identifier: MIT
 * MQTT worker shares HTTP scheduling, verified TLS and the global TCP quota.
 */
#include "aster_mqtt.h"
#include "aster_net_event.h"
#include "aster_http.h"
#include "aster_mem.h"
#include "aster_net.h"
#if defined(CONFIG_ASTER_HTTP_TLS)
#include "aster_http_tls.h"
#endif
#include <errno.h>
#include <string.h>
#ifndef ASTER_MQTT_HOST_TEST
#include <zephyr/kernel.h>
#endif
struct mqtt_filter { char name[ASTER_MQTT_TOPIC_MAX + 1]; unsigned qos; bool confirmed, active; };
struct mqtt_operation {
    uint32_t sequence;
    uint16_t id, filter;
    uint8_t kind, qos;
    bool sent, flushed, automatic;
    size_t length;
    uint8_t wire[ASTER_MQTT_PACKET_MAX];
};
struct mqtt_completion { uint32_t sequence; int32_t error; };
struct mqtt_client {
    uint64_t owner, transport_owner;
    uint32_t handle, transport, next_sequence, backoff_s;
    uint16_t packet_id;
    bool closing, cancel, disconnected, disconnect_sent, ping, tx_is_ping;
    bool connect_sent, output_operation;
    int64_t deadline, retry_at, last_tx, ping_at, operation_at, close_at, tls_started_ms;
    uint32_t tls_ms;
    struct aster_mqtt_connect options;
    char host[254], client[65], username[129], will_topic[129];
    uint8_t password[128], will[256];
    char *ca;
    size_t ca_length;
#if defined(CONFIG_ASTER_HTTP_TLS)
    struct aster_http_tls *tls;
    struct aster_http_tls_report tls_report;
#endif
    struct aster_mqtt_snapshot pub;
    uint32_t published_state;
    struct mqtt_filter filters[ASTER_MQTT_TOPICS];
    struct mqtt_operation operations[ASTER_MQTT_DEPTH];
    unsigned operation_count, receive_count, completion_cursor;
    struct mqtt_completion completions[ASTER_MQTT_COMPLETIONS];
    struct aster_mqtt_message messages[ASTER_MQTT_DEPTH];
    uint8_t input[ASTER_MQTT_PACKET_MAX], output[ASTER_MQTT_PACKET_MAX];
    size_t input_length, output_length, output_offset;
};
static struct mqtt_client *mqtt_clients[ASTER_NET_SLOTS];
static uint32_t mqtt_generation, mqtt_reconnects;
K_MUTEX_DEFINE(mqtt_lock);
static struct mqtt_client *mqtt_find(uint64_t owner, uint32_t handle)
{
    for (unsigned i = 0; i < ASTER_NET_SLOTS; ++i)
        if (mqtt_clients[i] && mqtt_clients[i]->owner == owner &&
            mqtt_clients[i]->handle == handle && !mqtt_clients[i]->closing)
            return mqtt_clients[i];
    return NULL;
}
static void mqtt_complete(struct mqtt_client *c, int error)
{
    if (!c->operation_count) return;
    struct mqtt_operation *o = &c->operations[0];
    if (o->kind == 2 || o->kind == 3) {
        struct mqtt_filter *f = &c->filters[o->filter];
        if (!error) {
            if (o->kind == 3) memset(f, 0, sizeof(*f));
            else { f->confirmed = true; f->active = true; }
        } else if (!f->confirmed) memset(f, 0, sizeof(*f));
    }
    if (!o->automatic) {
        c->pub.last_operation = o->sequence;
        c->pub.last_operation_kind = o->kind;
        c->pub.operation_error = error;
        c->completions[c->completion_cursor++ % ASTER_MQTT_COMPLETIONS] =
            (struct mqtt_completion){o->sequence, error};
    }
    --c->operation_count;
    memmove(c->operations, c->operations + 1,
             c->operation_count * sizeof(c->operations[0]));
}
static int mqtt_tls_drop(struct mqtt_client *c)
{
#if defined(CONFIG_ASTER_HTTP_TLS)
    return aster_http_tls_free(&c->tls);
#else
    (void)c; return 0;
#endif
}
static void mqtt_fault(struct mqtt_client *c, int error, bool permanent)
{
    c->pub.error = error;
    while (c->operation_count) mqtt_complete(c, error);
    for (unsigned i = 0; i < ASTER_MQTT_TOPICS; ++i) c->filters[i].active = false;
    c->input_length = c->output_length = c->output_offset = 0;
    c->ping = c->tx_is_ping = c->connect_sent = c->output_operation = false;
    c->disconnected = true;
    if (!permanent && c->options.reconnect) {
        c->pub.state = ASTER_MQTT_BACKOFF;
        c->retry_at = k_uptime_get() + c->backoff_s * 1000;
        c->backoff_s = c->backoff_s >= 16 ? 30 : c->backoff_s * 2;
    } else c->pub.state = c->cancel ? ASTER_MQTT_CANCELLED : ASTER_MQTT_FAILED;
}
static int mqtt_wire_send(struct mqtt_client *c, const void *data, size_t length)
{
#if defined(CONFIG_ASTER_HTTP_TLS)
    if (c->tls) return aster_http_tls_send(c->tls, data, length, &c->tls_report);
#endif
    return aster_net_send(c->transport_owner, c->transport, data, length, NULL, 0);
}
static int mqtt_wire_recv(struct mqtt_client *c, void *data, size_t length)
{
#if defined(CONFIG_ASTER_HTTP_TLS)
    if (c->tls) return aster_http_tls_recv(c->tls, data, length, &c->tls_report);
#endif
    size_t required;
    return aster_net_recv(c->transport_owner, c->transport, data, length, &required, NULL, NULL);
}
static int mqtt_flush(struct mqtt_client *c)
{
    if (c->output_offset == c->output_length) return 0;
    size_t length = c->output_length - c->output_offset;
    if (length > ASTER_NET_TCP_BUFFER) length = ASTER_NET_TCP_BUFFER;
    int rc = mqtt_wire_send(c, c->output + c->output_offset, length);
    if (rc < 0) return rc;
    if (!rc) return -EAGAIN;
    c->output_offset += rc;
    if (c->output_offset != c->output_length) return -EAGAIN;
    c->output_offset = c->output_length = 0; c->last_tx = k_uptime_get();
    if (c->tx_is_ping) { c->ping = true; c->ping_at = c->last_tx; c->tx_is_ping = false; }
    if (c->output_operation && c->operation_count) {
        c->operations[0].flushed = true;
        if (c->operations[0].kind == 1 && !c->operations[0].qos) {
            ++c->pub.sent; mqtt_complete(c, 0);
        }
    }
    c->output_operation = false;
    return 0;
}
static uint16_t mqtt_next_id(struct mqtt_client *c)
{
    if (++c->packet_id == 0) ++c->packet_id;
    return c->packet_id;
}
static int mqtt_queue_sub(struct mqtt_client *c, unsigned slot, unsigned qos,
                           bool unsubscribe, bool automatic)
{
    if (c->operation_count == ASTER_MQTT_DEPTH) return -EAGAIN;
    if (c->next_sequence == INT32_MAX) return -EOVERFLOW;
    struct mqtt_operation *o = &c->operations[c->operation_count];
    memset(o, 0, sizeof(*o)); o->id = mqtt_next_id(c);
    int rc = aster_mqtt_encode_sub(o->wire, sizeof(o->wire),
        c->filters[slot].name, qos, o->id, unsubscribe);
    if (rc < 0) return rc;
    o->length = rc; o->filter = slot; o->qos = qos;
    o->kind = unsubscribe ? 3 : 2; o->automatic = automatic;
    o->sequence = ++c->next_sequence; ++c->operation_count;
    return o->sequence;
}
static int mqtt_packet(struct mqtt_client *c, const struct aster_mqtt_packet *p)
{
    if (c->pub.state == ASTER_MQTT_CONNACK) {
        if (p->type != 2 || !c->connect_sent || c->output_length) return -EPROTO;
        c->pub.connack = p->body[1];
        if (p->body[1]) return -EACCES;
        c->pub.state = ASTER_MQTT_READY; c->pub.error = 0; c->backoff_s = 1;
        return 0;
    }
    if (c->pub.state != ASTER_MQTT_READY || p->type == 2) return -EPROTO;
    if (p->type == 13) {
        if (!c->ping) return -EPROTO;
        c->ping = false; return 0;
    }
    if (p->type == 3) {
        if (c->receive_count == ASTER_MQTT_DEPTH || (p->qos && c->output_length)) return -EAGAIN;
        struct aster_mqtt_message *m = &c->messages[c->receive_count++];
        memcpy(m->topic, p->topic, p->topic_length); m->topic[p->topic_length] = 0;
        if (p->payload_length) memcpy(m->data, p->payload, p->payload_length);
        m->length = p->payload_length; m->qos = p->qos; m->retain = p->retain; m->dup = p->dup;
        ++c->pub.received;
        if (p->qos) {
            c->output[0] = 0x40; c->output[1] = 2;
            c->output[2] = p->id >> 8; c->output[3] = p->id & 255;
            c->output_length = 4; c->output_offset = 0;
        }
        return 0;
    }
    if (!c->operation_count) return -EPROTO;
    struct mqtt_operation *o = &c->operations[0];
    if (!o->flushed || p->id != o->id) return -EPROTO;
    if (p->type == 4 && o->kind == 1 && o->qos == 1) {
        ++c->pub.sent; mqtt_complete(c, 0); return 0;
    }
    if (p->type == 9 && o->kind == 2) {
        if (p->body[2] != 0x80 && p->body[2] > o->qos) return -EPROTO;
        int error = p->body[2] == 0x80 ? -EACCES : 0;
        bool automatic = o->automatic;
        if (!error) c->filters[o->filter].qos = p->body[2];
        mqtt_complete(c, error);
        /* Avoid resubscribe loops if an existing filter is newly forbidden. */
        return error && automatic ? error : 0;
    }
    if (p->type == 11 && o->kind == 3) { mqtt_complete(c, 0); return 0; }
    return -EPROTO;
}
static void mqtt_process(struct mqtt_client *c)
{
    int64_t now = k_uptime_get();
    if (c->cancel && c->pub.state < ASTER_MQTT_FAILED) mqtt_fault(c, -ECANCELED, true);
    if (c->disconnected) {
        if (mqtt_tls_drop(c) == -EAGAIN) return;
        if (aster_net_finish(c->transport_owner, c->transport) == -EAGAIN) return;
        c->disconnected = false;
    }
    if (c->pub.state >= ASTER_MQTT_FAILED) return;
    if (c->pub.state == ASTER_MQTT_BACKOFF) {
        if (now < c->retry_at) return;
        int rc = aster_net_restart(c->transport_owner, c->transport,
            c->host, c->options.port, c->options.timeout_ms);
        if (rc == -EAGAIN || rc == -EBUSY) return;
        if (rc < 0) { mqtt_fault(c, rc, true); return; }
        c->transport = rc; ++c->pub.reconnects; ++mqtt_reconnects;
        c->deadline = now + c->options.timeout_ms;
        c->pub.state = ASTER_MQTT_CONNECTING;
    }
    if (c->pub.state < ASTER_MQTT_READY && now >= c->deadline) {
        mqtt_fault(c, -ETIMEDOUT, false); return;
    }
    if (c->pub.state == ASTER_MQTT_CONNECTING) {
        struct aster_net_snapshot t;
        int rc = aster_net_snapshot(c->transport_owner, c->transport, &t);
        if (rc == -EAGAIN) return;
        if (rc < 0 || t.state == ASTER_NET_FAILED || t.state == ASTER_NET_EOF) {
            mqtt_fault(c, rc < 0 ? rc : t.error ? t.error : -ECONNRESET, false); return;
        }
        if (t.state != ASTER_NET_READY) return;
        c->pub.state = c->options.tls ? ASTER_MQTT_HANDSHAKE : ASTER_MQTT_CONNACK;
        if (c->options.tls && !c->tls_started_ms) c->tls_started_ms = now;
    }
#if defined(CONFIG_ASTER_HTTP_TLS)
    if (c->pub.state == ASTER_MQTT_HANDSHAKE) {
        int rc = 0;
        if (!c->tls) rc = aster_http_tls_new(&c->tls, c->transport_owner,
            c->transport, c->host, c->ca, c->ca_length, &c->tls_report);
        if (!rc) rc = aster_http_tls_handshake(c->tls, &c->tls_report);
        c->pub.tls_verify = c->tls_report.verify;
        if (rc == -EAGAIN) return;
        if (rc) { mqtt_fault(c, rc, rc == -EACCES || rc == -EINVAL || rc == -ENODATA); return; }
        c->tls_ms = aster_net_diag_span_ms(c->tls_started_ms, k_uptime_get());
        c->pub.state = ASTER_MQTT_CONNACK;
    }
#endif
    if (c->pub.state == ASTER_MQTT_CONNACK && !c->connect_sent) {
        int rc = aster_mqtt_encode_connect(c->output, sizeof(c->output), &c->options);
        if (rc < 0) { mqtt_fault(c, rc, true); return; }
        c->output_length = rc; c->output_offset = 0; c->connect_sent = true;
    }
    int rc = mqtt_flush(c);
    if (rc < 0 && rc != -EAGAIN) { mqtt_fault(c, rc, false); return; }
    if (c->pub.state == ASTER_MQTT_READY) {
        if ((c->ping && now - c->ping_at >= (int64_t)c->options.keepalive_s * 1000) ||
            (c->operation_count && c->operations[0].sent && now - c->operation_at >= 10000)) {
            mqtt_fault(c, -ETIMEDOUT, false); return;
        }
        if (!c->output_length && !c->ping && now - c->last_tx >= (int64_t)c->options.keepalive_s * 1000) {
            c->output[0] = 0xc0; c->output[1] = 0;
            c->output_length = 2; c->tx_is_ping = true;
        }
        if (c->operation_count < ASTER_MQTT_DEPTH) {
            for (unsigned i = 0; i < ASTER_MQTT_TOPICS; ++i) {
                bool pending = false;
                for (unsigned j = 0; j < c->operation_count; ++j)
                    if (c->operations[j].kind != 1 && c->operations[j].filter == i) pending = true;
                if (c->filters[i].confirmed && !c->filters[i].active && !pending) {
                    rc = mqtt_queue_sub(c, i, c->filters[i].qos, false, true);
                    if (rc < 0) { mqtt_fault(c, rc, true); return; }
                    break;
                }
            }
        }
        if (!c->output_length && c->operation_count && !c->operations[0].sent) {
            struct mqtt_operation *o = &c->operations[0];
            memcpy(c->output, o->wire, o->length); c->output_length = o->length;
            o->sent = true; c->output_operation = true; c->operation_at = now;
        }
    }
    /* One complete packet/turn bounds work even with a chatty broker. */
    struct aster_mqtt_packet p;
    rc = aster_mqtt_decode(c->input, c->input_length, &p);
    if (!rc && c->input_length < sizeof(c->input)) {
        int n = mqtt_wire_recv(c, c->input + c->input_length,
                               sizeof(c->input) - c->input_length);
        if (n == -EAGAIN) return;
        if (n <= 0) { mqtt_fault(c, n < 0 ? n : -ECONNRESET, false); return; }
        c->input_length += n;
        rc = aster_mqtt_decode(c->input, c->input_length, &p);
    }
    if (rc < 0) { mqtt_fault(c, rc, true); return; }
    if (!rc) return;
    rc = mqtt_packet(c, &p);
    if (rc == -EAGAIN) return;
    if (rc) { mqtt_fault(c, rc, true); return; }
    c->input_length -= p.wire_length;
    memmove(c->input, c->input + p.wire_length, c->input_length);
}
/* Drain an already-partially-written packet before DISCONNECT, or abort on
 * deadline. Never splice a control packet into a partial MQTT/TLS frame. */
static bool mqtt_dispose(struct mqtt_client *c)
{
    int64_t now = k_uptime_get();
    if (!c->close_at) c->close_at = now + 1500;
    if (!c->cancel && c->pub.state == ASTER_MQTT_READY && now < c->close_at) {
        int rc = mqtt_flush(c);
        if (rc == -EAGAIN) return false;
        if (!rc && !c->disconnect_sent) {
            c->output[0] = 0xe0; c->output[1] = 0;
            c->output_length = 2; c->disconnect_sent = true;
            return false;
        }
        if (!rc) {
            struct aster_net_snapshot t;
            rc = aster_net_snapshot(c->transport_owner, c->transport, &t);
            if (rc == -EAGAIN || (!rc && t.tx_queued)) return false;
        }
    }
    if (mqtt_tls_drop(c) == -EAGAIN) return false;
    if (c->transport && aster_net_close(c->transport_owner, c->transport) == -EAGAIN) return false;
    if (c->ca && aster_http_svc_tls_release(&c->ca) == -EAGAIN) return false;
    /* Authentication material is not retained in freed allocator storage. */
    memset(c->password, 0, sizeof(c->password));
    return true;
}
bool aster_mqtt_svc_tick(void)
{
    bool active = false;
    k_mutex_lock(&mqtt_lock, K_FOREVER);
    for (unsigned i = 0; i < ASTER_NET_SLOTS; ++i) {
        struct mqtt_client *c = mqtt_clients[i];
        if (!c) continue;
        active = true;
        if (c->closing) {
            if (mqtt_dispose(c)) {
                uint32_t tls_ms = c->tls_ms;
                mqtt_clients[i] = NULL;
                aster_psram_free(c);
                aster_net_diag_sample(0, 0, tls_ms);
            }
            continue; /* c may be freed: never touch it below */
        }
        mqtt_process(c);
        /* N5.5: coalesced state change for this client's owning APP. */
        if (c->pub.state != c->published_state) {
            c->published_state = c->pub.state;
            aster_net_event_post(c->owner, c->handle, ASTER_NET_EVENT_MQTT, c->pub.state);
        }
    }
    k_mutex_unlock(&mqtt_lock);
    return active;
}
int aster_mqtt_svc_open(uint64_t owner, const struct aster_mqtt_connect *o)
{
    if (!owner || owner > INT64_MAX) return -EINVAL;
    int rc = aster_mqtt_connect_check(o);
    if (rc) return rc;
#if !defined(CONFIG_ASTER_HTTP_TLS)
    if (o->tls) return -ENOTSUP;
#endif
    rc = aster_http_svc_worker_wake();
    if (rc) return rc;
    if (k_mutex_lock(&mqtt_lock, K_NO_WAIT)) return -EAGAIN;
    unsigned slot;
    for (slot = 0; slot < ASTER_NET_SLOTS && mqtt_clients[slot]; ++slot) {}
    if (slot == ASTER_NET_SLOTS) { rc = -ENOSPC; goto out; }
    if (mqtt_generation == INT32_MAX) { rc = -EOVERFLOW; goto out; }
    struct mqtt_client *c = aster_psram_alloc(sizeof(*c));
    if (!c) { rc = -ENOMEM; goto out; }
    memset(c, 0, sizeof(*c)); c->owner = owner; c->options = *o;
    strcpy(c->host, o->host); strcpy(c->client, o->client_id);
    c->options.host = c->host; c->options.client_id = c->client;
    if (o->username) { strcpy(c->username, o->username); c->options.username = c->username; }
    if (o->password) { memcpy(c->password, o->password, o->password_length); c->options.password = c->password; }
    if (o->will_topic) { strcpy(c->will_topic, o->will_topic); c->options.will_topic = c->will_topic; }
    if (o->will_length) memcpy(c->will, o->will_payload, o->will_length);
    c->options.will_payload = c->will;
    if (o->tls) {
        rc = aster_http_svc_tls_acquire(&c->ca, &c->ca_length);
        if (rc) { aster_psram_free(c); goto out; }
    }
    c->handle = ++mqtt_generation;
    c->transport_owner = UINT64_C(0xc000000000000000) | c->handle;
    rc = aster_net_open(c->transport_owner, ASTER_NET_TCP, c->host,
                        o->port, o->timeout_ms);
    if (rc < 0) {
        /* Lease release can itself be EAGAIN. The worker completes teardown. */
        c->closing = true; c->cancel = true; mqtt_clients[slot] = c; goto out;
    }
    /* Use the globally monotonic transport namespace for the public handle.
     * A separate per-protocol counter could alias an APP-owned TCP/HTTP handle.
     * Keep this initial handle stable when reconnect replaces the transport. */
    c->handle = rc;
    c->transport = rc; c->pub.state = ASTER_MQTT_CONNECTING;
    c->deadline = k_uptime_get() + o->timeout_ms; c->backoff_s = 1;
    mqtt_clients[slot] = c; rc = c->handle;
out:
    k_mutex_unlock(&mqtt_lock);
    /* Publish the client before waking; the worker may run on the other CPU. */
    aster_http_svc_worker_signal();
    return rc;
}
int aster_mqtt_svc_publish(uint64_t owner, uint32_t handle, const char *topic,
 const void *data, size_t length, unsigned qos, bool retain)
{
    if (!topic || (!data && length) || qos > 1) return -EINVAL;
    if (length > ASTER_MQTT_PAYLOAD_MAX) return -EMSGSIZE;
    int rc = aster_mqtt_topic(topic, false);
    if (rc) return rc;
    if (k_mutex_lock(&mqtt_lock, K_NO_WAIT)) return -EAGAIN;
    struct mqtt_client *c = mqtt_find(owner, handle);
    rc = -EBADF;
    if (c) {
        rc = c->pub.state != ASTER_MQTT_READY ? -ENOTCONN :
             c->operation_count == ASTER_MQTT_DEPTH ? -EAGAIN : 0;
        if (!rc && c->next_sequence == INT32_MAX) rc = -EOVERFLOW;
        if (!rc) {
            struct mqtt_operation *o = &c->operations[c->operation_count];
            memset(o, 0, sizeof(*o)); o->id = mqtt_next_id(c);
            rc = aster_mqtt_encode_publish(o->wire, sizeof(o->wire), topic,
                                            data, length, qos, retain, o->id);
            if (rc > 0) {
                o->length = rc; o->kind = 1; o->qos = qos;
                o->sequence = ++c->next_sequence; ++c->operation_count; rc = o->sequence;
            }
        }
    }
    k_mutex_unlock(&mqtt_lock); return rc;
}
int aster_mqtt_svc_subscribe(uint64_t owner, uint32_t handle, const char *topic,
 unsigned qos, bool unsubscribe)
{
    if (qos > 1) return -EINVAL;
    int rc = aster_mqtt_topic(topic, true);
    if (rc) return rc;
    if (k_mutex_lock(&mqtt_lock, K_NO_WAIT)) return -EAGAIN;
    struct mqtt_client *c = mqtt_find(owner, handle); rc = -EBADF;
    if (c) {
        rc = c->pub.state != ASTER_MQTT_READY ? -ENOTCONN : 0;
        unsigned i, empty = ASTER_MQTT_TOPICS;
        for (i = 0; i < ASTER_MQTT_TOPICS; ++i) {
            if (!strcmp(c->filters[i].name, topic)) break;
            if (!c->filters[i].name[0]) empty = i;
        }
        if (!rc && i == ASTER_MQTT_TOPICS) {
            if (unsubscribe) rc = -ENOENT;
            else if (empty == ASTER_MQTT_TOPICS) rc = -ENOSPC;
            else i = empty;
        }
        if (!rc) {
            for (unsigned j = 0; j < c->operation_count; ++j)
                if (c->operations[j].kind != 1 && c->operations[j].filter == i) rc = -EBUSY;
        }
        if (!rc) {
            bool fresh = !c->filters[i].name[0];
            if (fresh) strcpy(c->filters[i].name, topic);
            rc = mqtt_queue_sub(c, i, qos, unsubscribe, false);
            if (rc < 0 && fresh) memset(&c->filters[i], 0, sizeof(c->filters[i]));
        }
    }
    k_mutex_unlock(&mqtt_lock); return rc;
}
int aster_mqtt_svc_status(uint64_t owner, uint32_t handle, struct aster_mqtt_snapshot *out)
{
    if (!out) return -EINVAL;
    if (k_mutex_lock(&mqtt_lock, K_NO_WAIT)) return -EAGAIN;
    struct mqtt_client *c = mqtt_find(owner, handle);
    if (c) {
        *out = c->pub; out->queued = c->receive_count; out->pending = c->operation_count;
#if defined(CONFIG_ASTER_HTTP_TLS)
        out->tls_error = c->tls_report.error;
        out->transport_error = c->tls_report.transport_error;
#endif
        out->subscriptions = 0;
        for (unsigned i = 0; i < ASTER_MQTT_TOPICS; ++i) out->subscriptions += c->filters[i].active;
    }
    k_mutex_unlock(&mqtt_lock); return c ? 0 : -EBADF;
}
int aster_mqtt_svc_operation(uint64_t owner, uint32_t handle, uint32_t sequence,
                             int32_t *error)
{
    if (!error || !sequence) return -EINVAL;
    if (k_mutex_lock(&mqtt_lock, K_NO_WAIT)) return -EAGAIN;
    struct mqtt_client *c = mqtt_find(owner, handle); int rc = -EBADF;
    if (c) {
        rc = -ENOENT;
        for (unsigned i = 0; i < ASTER_MQTT_COMPLETIONS; ++i)
            if (c->completions[i].sequence == sequence) { *error = c->completions[i].error; rc = 0; }
        for (unsigned i = 0; i < c->operation_count; ++i)
            if (!c->operations[i].automatic && c->operations[i].sequence == sequence) rc = -EAGAIN;
    }
    k_mutex_unlock(&mqtt_lock); return rc;
}
int aster_mqtt_svc_recv(uint64_t owner, uint32_t handle, struct aster_mqtt_message *out)
{
    if (!out) return -EINVAL;
    if (k_mutex_lock(&mqtt_lock, K_NO_WAIT)) return -EAGAIN;
    struct mqtt_client *c = mqtt_find(owner, handle); int rc = -EBADF;
    if (c) {
        if (c->receive_count) {
            *out = c->messages[0]; --c->receive_count;
            memmove(c->messages, c->messages + 1, c->receive_count * sizeof(c->messages[0])); rc = 0;
        } else rc = c->pub.state >= ASTER_MQTT_FAILED ? c->pub.error : -EAGAIN;
    }
    k_mutex_unlock(&mqtt_lock); return rc;
}
static int mqtt_end(uint64_t owner, uint32_t handle, bool close)
{
    if (k_mutex_lock(&mqtt_lock, K_NO_WAIT)) return -EAGAIN;
    struct mqtt_client *c = mqtt_find(owner, handle);
    if (c) {
        if (close) c->closing = true;
        else {
            c->cancel = true;
            if (c->pub.state < ASTER_MQTT_FAILED) mqtt_fault(c, -ECANCELED, true);
        }
    }
    k_mutex_unlock(&mqtt_lock); return c ? 0 : -EBADF;
}
int aster_mqtt_svc_close(uint64_t o, uint32_t h) { return mqtt_end(o, h, true); }
int aster_mqtt_svc_cancel(uint64_t o, uint32_t h) { return mqtt_end(o, h, false); }
int aster_mqtt_svc_close_owner(uint64_t owner)
{
    if (k_mutex_lock(&mqtt_lock, K_NO_WAIT)) return -EAGAIN;
    for (unsigned i = 0; i < ASTER_NET_SLOTS; ++i)
        if (mqtt_clients[i] && mqtt_clients[i]->owner == owner) mqtt_clients[i]->closing = true;
    k_mutex_unlock(&mqtt_lock); return 0;
}
int aster_mqtt_svc_stats(struct aster_mqtt_stats *out)
{
    if (!out) return -EINVAL;
    if (k_mutex_lock(&mqtt_lock, K_NO_WAIT)) return -EAGAIN;
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < ASTER_NET_SLOTS; ++i) {
        struct mqtt_client *c = mqtt_clients[i];
        if (!c) continue;
        ++out->active; out->closing += c->closing; out->tls_active += c->ca != NULL;
        out->psram_bytes += sizeof(*c) + c->ca_length;
#if defined(CONFIG_ASTER_HTTP_TLS)
        if (c->tls) out->psram_bytes += aster_http_tls_context_bytes();
#endif
    }
    k_mutex_unlock(&mqtt_lock); return 0;
}
uint32_t aster_mqtt_svc_reconnects(void) { return mqtt_reconnects; }
