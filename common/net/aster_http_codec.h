/* SPDX-License-Identifier: MIT */
#ifndef ASTER_HTTP_CODEC_H
#define ASTER_HTTP_CODEC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ASTER_HTTP_URL_MAX 1024
#define ASTER_HTTP_HEADERS_MAX 2048
#define ASTER_HTTP_HEADER_COUNT 32
#define ASTER_HTTP_UPLOAD_MAX 2048
#define ASTER_HTTP_BODY_BUFFER 2048
#define ASTER_HTTP_REDIRECT_MAX 3
struct aster_http_url {
    bool tls;
    uint16_t port;
    char host[254], path[1025];
};
struct aster_http_field {
    uint16_t name, name_len, value, value_len;
};
struct aster_http_headers {
    struct aster_http_field fields[ASTER_HTTP_HEADER_COUNT];
    char data[ASTER_HTTP_HEADERS_MAX];
    uint16_t count, used;
};
enum aster_http_decode_state {
    HC_STATUS,
    HC_HEADERS,
    HC_FIXED,
    HC_EOF,
    HC_CHUNK_SIZE,
    HC_CHUNK_DATA,
    HC_CHUNK_CR,
    HC_CHUNK_LF,
    HC_TRAILERS,
    HC_DONE
};
struct aster_http_codec {
    struct aster_http_headers headers;
    enum aster_http_decode_state state;
    uint16_t status, line_len, metadata_bytes;
    uint8_t interim;
    bool head, headers_ready, chunked, has_length, cr;
    uint64_t content_length, remaining, body_bytes, body_limit;
    char line[ASTER_HTTP_HEADERS_MAX + 1];
    int error;
};
int aster_http_url_parse(const char *text, struct aster_http_url *out);
int aster_http_redirect(const struct aster_http_url *from, const char *location,
                        struct aster_http_url *out);
bool aster_http_same_origin(const struct aster_http_url *a,
                            const struct aster_http_url *b);
int aster_http_validate_headers(const char *text, size_t length);
void aster_http_codec_init(struct aster_http_codec *c, bool head,
                           uint64_t limit);
/* Body output is bounded; consumed may be less than input when full/done. */
int aster_http_codec_feed(struct aster_http_codec *c, const uint8_t *data,
                          size_t length, size_t *consumed, uint8_t *body,
                          size_t capacity, size_t *written);
int aster_http_codec_eof(struct aster_http_codec *c);
/* length excludes NUL; EMSGSIZE retains output and reports required length. */
int aster_http_field_get(const struct aster_http_headers *h, const char *name,
                         unsigned index, char *out, size_t capacity,
                         size_t *length);
#endif
