/* SPDX-License-Identifier: MIT
 * Bounded MQTT 3.1.1 framing. No allocator or platform socket dependency.
 */
#include "aster_mqtt_codec.h"
#include <errno.h>
#include <string.h>
static int utf8(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n;) {
        uint32_t c = p[i++], minimum = 0;
        unsigned more = 0;
        if (c >= 0xc2 && c <= 0xdf) { c &= 31; more = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 15; more = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { c &= 7; more = 3; minimum = 0x10000; }
        else if (c >= 0x80) return -EINVAL;
        if (more > n - i) return -EINVAL;
        while (more--) {
            if ((p[i] & 0xc0) != 0x80) return -EINVAL;
            c = (c << 6) | (p[i++] & 63);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
            c == 0 || c < 0x20 || (c >= 0x7f && c <= 0x9f) ||
            (c >= 0xfdd0 && c <= 0xfdef) || (c & 0xffff) >= 0xfffe)
            return -EINVAL;
    }
    return 0;
}
static int text(const char *s, size_t maximum, bool empty)
{
    if (!s) return empty ? 0 : -EINVAL;
    size_t n = strnlen(s, maximum + 1);
    if (n > maximum) return -EMSGSIZE;
    return !n && !empty ? -EINVAL : utf8((const uint8_t *)s, n);
}
int aster_mqtt_topic(const char *s, bool filter)
{
    int rc = text(s, ASTER_MQTT_TOPIC_MAX, false);
    if (rc) return rc;
    for (size_t i = 0; s[i]; ++i) {
        if (s[i] != '#' && s[i] != '+') continue;
        if (!filter || (i && s[i - 1] != '/') ||
            (s[i] == '#' ? s[i + 1] != 0 : s[i + 1] && s[i + 1] != '/'))
            return -EINVAL;
    }
    return 0;
}
int aster_mqtt_connect_check(const struct aster_mqtt_connect *o)
{
    if (!o || text(o->host, 253, false) || text(o->client_id, 64, false) ||
        text(o->username, 128, true) || !o->port || o->port > 65535 ||
        o->timeout_ms < 100 || o->timeout_ms > 30000 ||
        o->keepalive_s < 5 || o->keepalive_s > 300 || o->password_length > 128 ||
        (o->password_length && !o->password) || (o->password && !o->username) ||
        o->will_length > 256 || (o->will_length && !o->will_payload) ||
        o->will_qos > 1 || (!o->will_topic && (o->will_length || o->will_qos || o->will_retain)))
        return -EINVAL;
    /* IPv4/DNS only. A host is not a URL or a source of request delimiters. */
    for (const unsigned char *p = (const unsigned char *)o->host; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return -EINVAL;
    return o->will_topic ? aster_mqtt_topic(o->will_topic, false) : 0;
}
struct writer { uint8_t *p; size_t used, cap; bool bad; };
static void put(struct writer *w, const void *p, size_t n)
{
    if (n > w->cap - w->used) { w->bad = true; return; }
    if (n) memcpy(w->p + w->used, p, n);
    w->used += n;
}
static void word(struct writer *w, unsigned v)
{
    uint8_t p[] = {v >> 8, v & 255}; put(w, p, 2);
}
static void field(struct writer *w, const void *p, size_t n)
{
    word(w, n); put(w, p, n);
}
static int finish(struct writer *w, unsigned command)
{
    if (w->bad) return -EMSGSIZE;
    size_t n = w->used - 5, remaining = n;
    uint8_t head[5] = {command}; unsigned h = 1;
    do { head[h] = remaining % 128; remaining /= 128;
         if (remaining) head[h] |= 128;
         ++h; } while (remaining);
    memmove(w->p + h, w->p + 5, n); memcpy(w->p, head, h);
    return (int)(n + h);
}
int aster_mqtt_encode_connect(uint8_t *out, size_t cap, const struct aster_mqtt_connect *o)
{
    int rc = aster_mqtt_connect_check(o);
    if (rc) return rc;
    if (!out || cap < 5) return -EMSGSIZE;
    struct writer w = {out, 5, cap, false};
    const uint8_t prefix[] = {0, 4, 'M', 'Q', 'T', 'T', 4};
    uint8_t flags = 2 | (o->username ? 128 : 0) | (o->password ? 64 : 0) |
        (o->will_topic ? 4 | (o->will_qos << 3) | (o->will_retain ? 32 : 0) : 0);
    put(&w, prefix, sizeof(prefix)); put(&w, &flags, 1); word(&w, o->keepalive_s);
    field(&w, o->client_id, strlen(o->client_id));
    if (o->will_topic) {
        field(&w, o->will_topic, strlen(o->will_topic));
        field(&w, o->will_payload, o->will_length);
    }
    if (o->username) field(&w, o->username, strlen(o->username));
    if (o->password) field(&w, o->password, o->password_length);
    return finish(&w, 0x10);
}
int aster_mqtt_encode_publish(uint8_t *out, size_t cap, const char *topic,
 const void *data, size_t n, unsigned qos, bool retain, uint16_t id)
{
    int rc = aster_mqtt_topic(topic, false);
    if (rc) return rc;
    if (n > ASTER_MQTT_PAYLOAD_MAX) return -EMSGSIZE;
    if ((!data && n) || qos > 1 || (qos && !id)) return -EINVAL;
    if (!out || cap < 5) return -EMSGSIZE;
    struct writer w = {out, 5, cap, false};
    field(&w, topic, strlen(topic)); if (qos) word(&w, id);
    put(&w, data, n); return finish(&w, 0x30 | (qos << 1) | retain);
}
int aster_mqtt_encode_sub(uint8_t *out, size_t cap, const char *topic,
 unsigned qos, uint16_t id, bool unsubscribe)
{
    int rc = aster_mqtt_topic(topic, true);
    if (rc) return rc;
    if (!id || qos > 1) return -EINVAL;
    if (!out || cap < 5) return -EMSGSIZE;
    struct writer w = {out, 5, cap, false};
    word(&w, id); field(&w, topic, strlen(topic));
    uint8_t q = qos; if (!unsubscribe) put(&w, &q, 1);
    return finish(&w, unsubscribe ? 0xa2 : 0x82);
}
int aster_mqtt_decode(const uint8_t *p, size_t n, struct aster_mqtt_packet *o)
{
    if (!p || !o) return -EINVAL;
    if (n < 2) return 0;
    size_t remaining = 0, multiplier = 1, h = 1;
    for (;;) {
        if (h >= n) return 0;
        uint8_t b = p[h++]; remaining += (b & 127) * multiplier;
        if (!(b & 128)) {
            if (h > 2 && !(b & 127)) return -EPROTO;
            break;
        }
        if (h == 5) return -EPROTO;
        multiplier *= 128;
    }
    if (remaining > ASTER_MQTT_PACKET_MAX - h) return -EMSGSIZE;
    unsigned type = p[0] >> 4, flags = p[0] & 15;
    if (type != 3 && flags) return -EPROTO;
    if (type != 2 && type != 3 && type != 4 && type != 9 && type != 11 && type != 13)
        return -EPROTO;
    if ((type == 2 && remaining != 2) || (type == 4 && remaining != 2) ||
        (type == 9 && remaining != 3) || (type == 11 && remaining != 2) ||
        (type == 13 && remaining)) return -EPROTO;
    if (n < h + remaining) return 0;
    memset(o, 0, sizeof(*o)); o->type = type; o->body = p + h;
    o->body_length = remaining; o->wire_length = h + remaining;
    if (type == 3) {
        o->qos = (flags >> 1) & 3; o->retain = flags & 1; o->dup = flags & 8;
        if (o->qos > 1 || (!o->qos && o->dup) || remaining < 2) return -EPROTO;
        size_t t = ((unsigned)o->body[0] << 8) | o->body[1], used = 2 + t;
        if (!t || t > ASTER_MQTT_TOPIC_MAX) return -EMSGSIZE;
        if (used + (o->qos ? 2 : 0) > remaining) return -EPROTO;
        if (utf8(o->body + 2, t)) return -EPROTO;
        for (size_t i = 0; i < t; ++i)
            if (o->body[2 + i] == '+' || o->body[2 + i] == '#') return -EPROTO;
        o->topic = o->body + 2; o->topic_length = t;
        if (o->qos) { o->id = ((unsigned)o->body[used] << 8) | o->body[used + 1]; used += 2;
            if (!o->id) return -EPROTO; }
        o->payload = o->body + used; o->payload_length = remaining - used;
        if (o->payload_length > ASTER_MQTT_PAYLOAD_MAX) return -EMSGSIZE;
    } else if (type == 4 || type == 9 || type == 11) {
        o->id = ((unsigned)o->body[0] << 8) | o->body[1];
        if (!o->id) return -EPROTO;
        if (type == 9 && o->body[2] != 0 && o->body[2] != 1 && o->body[2] != 0x80)
            return -EPROTO;
    } else if (type == 2 && (o->body[0] || o->body[1] > 5)) return -EPROTO;
    return (int)o->wire_length;
}
