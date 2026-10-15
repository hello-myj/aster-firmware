/* SPDX-License-Identifier: MIT
 * Bounded incremental HTTP/1.x response decoding. No allocation or socket I/O.
 */
#include "aster_http_codec.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
static bool token(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}
static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static int integer(const char *s, size_t n, unsigned base, uint64_t *out)
{
    if (!n)
        return -EPROTO;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        int d = base == 16 ? hex_digit(s[i])
                           : (s[i] >= '0' && s[i] <= '9' ? s[i] - '0' : -1);
        if (d < 0 || (unsigned)d >= base ||
            v > (UINT64_MAX - (unsigned)d) / base)
            return -EPROTO;
        v = v * base + d;
    }
    *out = v;
    return 0;
}
int aster_http_url_parse(const char *text, struct aster_http_url *out)
{
    if (!text || !out)
        return -EINVAL;
    size_t n = strnlen(text, ASTER_HTTP_URL_MAX + 1);
    if (!n || n > ASTER_HTTP_URL_MAX)
        return -EMSGSIZE;
    memset(out, 0, sizeof(*out));
    const char *s;
    if (!strncasecmp(text, "http://", 7)) {
        s = text + 7;
        out->port = 80;
    } else if (!strncasecmp(text, "https://", 8)) {
        s = text + 8;
        out->tls = true;
        out->port = 443;
    } else
        return -ENOTSUP;
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = text[i];
        if (ch <= 32 || ch >= 127 || ch == '\\')
            return -EINVAL;
    }
    const char *end = s + strcspn(s, "/?#"), *colon = memchr(s, ':', end - s);
    size_t host_len = (colon ? colon : end) - s;
    if (!host_len || host_len > 253)
        return -EINVAL;
    for (size_t i = 0; i < host_len; ++i) {
        unsigned char ch = s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '.'))
            return -EINVAL;
        out->host[i] = ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
    }
    if (colon) {
        uint64_t port;
        if (integer(colon + 1, end - colon - 1, 10, &port) || !port ||
            port > 65535)
            return -EINVAL;
        out->port = port;
    }
    const char *fragment = strchr(end, '#');
    size_t path_len = (fragment ? fragment : text + n) - end;
    unsigned prefix = *end == '/' ? 0 : 1;
    if (path_len + prefix > ASTER_HTTP_URL_MAX)
        return -EMSGSIZE;
    if (prefix)
        out->path[0] = '/';
    memcpy(out->path + prefix, end, path_len);
    out->path[path_len + prefix] = 0;
    return 0;
}
bool aster_http_same_origin(const struct aster_http_url *a,
                            const struct aster_http_url *b)
{
    return a->tls == b->tls && a->port == b->port &&
           !strcasecmp(a->host, b->host);
}
static int normalize(struct aster_http_url *url)
{
    char path[ASTER_HTTP_URL_MAX + 1];
    size_t used = 0;
    const char *query = strchr(url->path, '?');
    size_t n = query ? (size_t)(query - url->path) : strlen(url->path);
    for (size_t pos = 0; pos < n;) {
        size_t end = pos + 1;
        while (end < n && url->path[end] != '/')
            ++end;
        size_t len = end - pos;
        if (len == 2 && !memcmp(url->path + pos, "/.", 2)) {
            if (end == n && (!used || path[used - 1] != '/'))
                path[used++] = '/';
        } else if (len == 3 && !memcmp(url->path + pos, "/..", 3)) {
            while (used && path[used - 1] != '/')
                --used;
            if (used)
                --used;
            if (end == n)
                path[used++] = '/';
        } else {
            memcpy(path + used, url->path + pos, len);
            used += len;
        }
        pos = end;
    }
    if (!used)
        path[used++] = '/';
    size_t q = query ? strlen(query) : 0;
    if (used + q > ASTER_HTTP_URL_MAX)
        return -EMSGSIZE;
    if (q)
        memcpy(path + used, query, q);
    path[used + q] = 0;
    strcpy(url->path, path);
    return 0;
}
int aster_http_redirect(const struct aster_http_url *from, const char *location,
                        struct aster_http_url *out)
{
    if (!from || !location || !location[0] || !out)
        return -EINVAL;
    char text[ASTER_HTTP_URL_MAX + 1];
    int n;
    if (!strncasecmp(location, "http://", 7) ||
        !strncasecmp(location, "https://", 8)) {
        int rc = aster_http_url_parse(location, out);
        return rc ? rc : normalize(out);
    }
    size_t first = strcspn(location, "/?#");
    if (memchr(location, ':', first))
        return -ENOTSUP;
    if (location[0] == '/' && location[1] == '/')
        n = snprintf(text, sizeof(text), "%s:%s", from->tls ? "https" : "http",
                     location);
    else {
        char prefix[ASTER_HTTP_URL_MAX + 1];
        strcpy(prefix, from->path);
        if (location[0] == '/')
            prefix[0] = 0;
        else if (location[0] == '?') {
            char *q = strchr(prefix, '?');
            if (q)
                *q = 0;
        } else if (location[0] ==
                   '#') { /* Retain the existing query; fragment is not sent. */
        } else {
            char *q = strchr(prefix, '?');
            if (q)
                *q = 0;
            char *last = strrchr(prefix, '/');
            if (last)
                last[1] = 0;
            else
                strcpy(prefix, "/");
        }
        n = snprintf(text, sizeof(text), "%s://%s:%u%s%s",
                     from->tls ? "https" : "http", from->host, from->port,
                     prefix, location);
    }
    if (n < 0 || (size_t)n >= sizeof(text))
        return -EMSGSIZE;
    int rc = aster_http_url_parse(text, out);
    return rc ? rc : normalize(out);
}
static bool eq(const char *s, size_t n, const char *word)
{
    return strlen(word) == n && !strncasecmp(s, word, n);
}
static int header_parts(const char *s, size_t n, size_t *name, size_t *value,
                        size_t *length)
{
    const char *colon = memchr(s, ':', n);
    if (!colon || colon == s)
        return -EPROTO;
    *name = colon - s;
    for (size_t i = 0; i < *name; ++i)
        if (!token((unsigned char)s[i]))
            return -EPROTO;
    size_t v = *name + 1, end = n;
    while (v < end && (s[v] == ' ' || s[v] == '\t'))
        ++v;
    while (end > v && (s[end - 1] == ' ' || s[end - 1] == '\t'))
        --end;
    for (size_t i = v; i < end; ++i)
        if (((unsigned char)s[i] < 32 && s[i] != '\t') ||
            (unsigned char)s[i] >= 127)
            return -EPROTO;
    *value = v;
    *length = end - v;
    return 0;
}
int aster_http_validate_headers(const char *text, size_t length)
{
    if ((!text && length) || length > ASTER_HTTP_HEADERS_MAX)
        return -EINVAL;
    for (size_t pos = 0; pos < length;) {
        size_t end = pos;
        while (end + 1 < length && memcmp(text + end, "\r\n", 2))
            ++end;
        if (end + 1 >= length || end == pos)
            return -EINVAL;
        size_t name, v, n;
        if (header_parts(text + pos, end - pos, &name, &v, &n))
            return -EINVAL;
        const char *s = text + pos;
        if (eq(s, name, "Host") || eq(s, name, "Content-Length") ||
            eq(s, name, "Transfer-Encoding") || eq(s, name, "Connection") ||
            eq(s, name, "Accept-Encoding") || eq(s, name, "Expect") ||
            eq(s, name, "Upgrade"))
            return -EINVAL;
        pos = end + 2;
    }
    return 0;
}
void aster_http_codec_init(struct aster_http_codec *c, bool head,
                           uint64_t limit)
{
    memset(c, 0, sizeof(*c));
    c->state = HC_STATUS;
    c->head = head;
    c->body_limit = limit;
}
static int field_line(struct aster_http_codec *c, bool trailer)
{
    size_t name, v, n;
    int rc = header_parts(c->line, c->line_len, &name, &v, &n);
    if (rc)
        return rc;
    bool cl = eq(c->line, name, "Content-Length"),
         te = eq(c->line, name, "Transfer-Encoding");
    if (trailer &&
        (cl || te || eq(c->line, name, "Host") ||
         eq(c->line, name, "Connection") || eq(c->line, name, "Trailer")))
        return -EPROTO;
    if (cl) {
        if (c->has_length || c->chunked)
            return -EPROTO;
        rc = integer(c->line + v, n, 10, &c->content_length);
        if (rc)
            return rc;
        c->has_length = true;
    }
    if (te) {
        if (c->chunked || c->has_length || !eq(c->line + v, n, "chunked"))
            return -EPROTO;
        c->chunked = true;
    }
    struct aster_http_headers *h = &c->headers;
    if (h->count == ASTER_HTTP_HEADER_COUNT ||
        name + n > sizeof(h->data) - h->used)
        return -EMSGSIZE;
    struct aster_http_field *f = &h->fields[h->count++];
    *f = (struct aster_http_field){h->used, name, h->used + name, n};
    memcpy(h->data + h->used, c->line, name);
    memcpy(h->data + h->used + name, c->line + v, n);
    h->used += name + n;
    return 0;
}
static int line_done(struct aster_http_codec *c)
{
    char *s = c->line;
    size_t n = c->line_len;
    s[n] = 0;
    switch (c->state) {
    case HC_STATUS:
        if (n < 12 ||
            (memcmp(s, "HTTP/1.0 ", 9) && memcmp(s, "HTTP/1.1 ", 9)) ||
            s[9] < '1' || s[9] > '5' || s[10] < '0' || s[10] > '9' ||
            s[11] < '0' || s[11] > '9' || (n > 12 && s[12] != ' '))
            return -EPROTO;
        c->status = (s[9] - '0') * 100 + (s[10] - '0') * 10 + s[11] - '0';
        c->state = HC_HEADERS;
        break;
    case HC_HEADERS:
        if (n)
            return field_line(c, false);
        if (c->status < 200) {
            if (c->status == 101 || ++c->interim > 4)
                return -EPROTO;
            memset(&c->headers, 0, sizeof(c->headers));
            c->has_length = c->chunked = false;
            c->content_length = 0;
            c->state = HC_STATUS;
            break;
        }
        c->headers_ready = true;
        if (c->head || c->status == 204 || c->status == 304) {
            c->state = HC_DONE;
            break;
        }
        if (c->has_length && c->content_length > c->body_limit)
            return -EMSGSIZE;
        c->remaining = c->content_length;
        c->state = c->chunked      ? HC_CHUNK_SIZE
                   : c->has_length ? (c->remaining ? HC_FIXED : HC_DONE)
                                   : HC_EOF;
        break;
    case HC_CHUNK_SIZE: {
        char *semi = memchr(s, ';', n);
        size_t digits = semi ? (size_t)(semi - s) : n;
        if (digits > 16 || integer(s, digits, 16, &c->remaining))
            return -EPROTO;
        if (semi) {
            if (digits + 1 == n)
                return -EPROTO;
            for (size_t i = digits + 1; i < n; ++i)
                if ((unsigned char)s[i] < 32 || (unsigned char)s[i] >= 127)
                    return -EPROTO;
        }
        if (c->remaining > c->body_limit - c->body_bytes)
            return -EMSGSIZE;
        c->state = c->remaining ? HC_CHUNK_DATA : HC_TRAILERS;
        break;
    }
    case HC_TRAILERS:
        if (n)
            return field_line(c, true);
        c->state = HC_DONE;
        break;
    default:
        return -EPROTO;
    }
    return 0;
}
int aster_http_codec_feed(struct aster_http_codec *c, const uint8_t *data,
                          size_t length, size_t *consumed, uint8_t *body,
                          size_t capacity, size_t *written)
{
    if (!c || (!data && length) || (!body && capacity) || !consumed || !written)
        return -EINVAL;
    *consumed = *written = 0;
    if (c->error)
        return c->error;
    while (*consumed < length && c->state != HC_DONE) {
        if (c->state == HC_FIXED || c->state == HC_EOF ||
            c->state == HC_CHUNK_DATA) {
            size_t n = length - *consumed;
            if (n > capacity - *written)
                n = capacity - *written;
            if (c->state != HC_EOF && n > c->remaining)
                n = c->remaining;
            if (!n)
                break;
            if (n > c->body_limit - c->body_bytes) {
                c->error = -EMSGSIZE;
                break;
            }
            memcpy(body + *written, data + *consumed, n);
            *consumed += n;
            *written += n;
            c->body_bytes += n;
            if (c->state != HC_EOF) {
                c->remaining -= n;
                if (!c->remaining)
                    c->state = c->state == HC_FIXED ? HC_DONE : HC_CHUNK_CR;
            }
            continue;
        }
        unsigned char ch = data[(*consumed)++];
        if (c->state == HC_CHUNK_CR) {
            if (ch != '\r')
                c->error = -EPROTO;
            else
                c->state = HC_CHUNK_LF;
        } else if (c->state == HC_CHUNK_LF) {
            if (ch != '\n')
                c->error = -EPROTO;
            else
                c->state = HC_CHUNK_SIZE;
        } else {
            if (c->state != HC_CHUNK_SIZE &&
                ++c->metadata_bytes > ASTER_HTTP_HEADERS_MAX) {
                c->error = -EMSGSIZE;
                break;
            }
            if (c->state == HC_CHUNK_SIZE && c->line_len >= 128) {
                c->error = -EMSGSIZE;
                break;
            }
            if (c->cr) {
                if (ch != '\n')
                    c->error = -EPROTO;
                else {
                    c->cr = false;
                    c->error = line_done(c);
                    c->line_len = 0;
                }
            } else if (ch == '\r')
                c->cr = true;
            else if (ch == '\n' || ch == 0 || (ch < 32 && ch != '\t') ||
                     ch >= 127)
                c->error = -EPROTO;
            else if (c->line_len == sizeof(c->line) - 1)
                c->error = -EMSGSIZE;
            else
                c->line[c->line_len++] = ch;
        }
        if (c->error)
            break;
    }
    return c->error;
}
int aster_http_codec_eof(struct aster_http_codec *c)
{
    if (!c)
        return -EINVAL;
    if (c->error)
        return c->error;
    if (c->state == HC_EOF)
        c->state = HC_DONE;
    return c->state == HC_DONE ? 0 : (c->error = -EPROTO);
}
int aster_http_field_get(const struct aster_http_headers *h, const char *name,
                         unsigned index, char *out, size_t capacity,
                         size_t *length)
{
    if (!h || !name || (!out && capacity) || !length)
        return -EINVAL;
    *length = 0;
    for (unsigned i = 0; i < h->count; ++i) {
        const struct aster_http_field *f = &h->fields[i];
        if (eq(h->data + f->name, f->name_len, name) && !index--) {
            *length = f->value_len;
            if (capacity < f->value_len)
                return -EMSGSIZE;
            if (f->value_len)
                memcpy(out, h->data + f->value, f->value_len);
            return 0;
        }
    }
    return -ENOENT;
}
