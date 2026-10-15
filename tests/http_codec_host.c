/* SPDX-License-Identifier: MIT */
#include "aster_http_codec.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void decode(const char *wire, bool head, const char *expect,
                   unsigned status)
{
    for (size_t step = 1; step <= strlen(wire); ++step) {
        struct aster_http_codec c;
        aster_http_codec_init(&c, head, 1048576);
        char body[2048];
        size_t total = 0;
        for (size_t pos = 0; pos < strlen(wire);) {
            size_t n = strlen(wire) - pos;
            if (n > step)
                n = step;
            size_t used, wrote;
            int rc = aster_http_codec_feed(&c, (void *)(wire + pos), n, &used,
                                           (void *)(body + total),
                                           sizeof(body) - total, &wrote);
            assert(!rc);
            total += wrote;
            pos += used;
            if (c.state == HC_DONE)
                break;
            assert(used);
        }
        assert(!aster_http_codec_eof(&c));
        assert(c.status == status && total == strlen(expect));
        assert(!memcmp(body, expect, total));
    }
}
static void bad(const char *wire, bool eof, int expected)
{
    struct aster_http_codec c;
    aster_http_codec_init(&c, false, 10000);
    uint8_t body[1000];
    size_t used, wrote;
    int rc = aster_http_codec_feed(&c, (void *)wire, strlen(wire), &used, body,
                                   sizeof(body), &wrote);
    if (!rc && eof)
        rc = aster_http_codec_eof(&c);
    assert(rc == expected);
}
int main(void)
{
    decode(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-A: a\r\nX-A: b\r\n\r\nhello",
        false, "hello", 200);
    decode("HTTP/1.0 404 Not Found\r\nX-C: z\r\n\r\nmissing", false, "missing",
           404);
    decode(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: "
        "chunked\r\n\r\n2;foo=bar\r\nhe\r\n3\r\nllo\r\n0\r\nX-T: done\r\n\r\n",
        false, "hello", 200);
    decode("HTTP/1.1 103 Early Hints\r\nLink: a\r\n\r\nHTTP/1.1 100 "
           "Continue\r\n\r\nHTTP/1.1 500 Error\r\nContent-Length: 1\r\n\r\nx",
           false, "x", 500);
    decode("HTTP/1.1 204 No Content\r\n\r\n", false, "", 204);
    decode("HTTP/1.1 304 Not Modified\r\nContent-Length: 999999999\r\n\r\n",
           false, "", 304);
    decode("HTTP/1.1 200 OK\r\nContent-Length: 999999999\r\n\r\n", true, "",
           200);
    bad("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc", true, -EPROTO);
    bad("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nab", true,
        -EPROTO);
    bad("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
        false, -EPROTO);
    bad("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: "
        "chunked\r\n\r\n",
        false, -EPROTO);
    bad("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", false,
        -EPROTO);
    bad("HTTP/1.1 200 OK\n\n", false, -EPROTO);
    bad("HTTP/1.1 101 Switching Protocols\r\n\r\n", false, -EPROTO);
    bad("HTTP/1.1 200 OK\r\nTransfer-Encoding: "
        "chunked\r\n\r\n0\r\nContent-Length: 0\r\n\r\n",
        false, -EPROTO);
    bad("HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n",
        false, -EPROTO);
    struct aster_http_url a, b;
    assert(
        !aster_http_url_parse("https://Example.COM:8443/a/b?old#fragment", &a));
    assert(a.tls && a.port == 8443 && !strcmp(a.host, "example.com") &&
           !strcmp(a.path, "/a/b?old"));
    assert(!aster_http_redirect(&a, "../c?q", &b) && !strcmp(b.path, "/c?q") &&
           aster_http_same_origin(&a, &b));
    assert(!aster_http_redirect(&a, "?new", &b) && !strcmp(b.path, "/a/b?new"));
    assert(!aster_http_redirect(&a, "#anchor", &b) &&
           !strcmp(b.path, "/a/b?old"));
    assert(!aster_http_redirect(&a, "//other:80/", &b) && b.tls &&
           !aster_http_same_origin(&a, &b));
    assert(aster_http_url_parse("http://user:pass@example/", &b) < 0);
    assert(aster_http_url_parse("http://example:0/", &b) < 0);
    assert(aster_http_url_parse("http://[::1]/", &b) < 0);
    assert(aster_http_redirect(&a, "mailto:x", &b) == -ENOTSUP);
    assert(!aster_http_redirect(&a, "/next?return=http://other/x", &b));
    assert(!strcmp(b.path, "/next?return=http://other/x"));
    assert(!aster_http_url_parse("HTTPS://EXAMPLE.com/Case", &b));
    assert(b.tls && !strcmp(b.host,"example.com") && !strcmp(b.path,"/Case"));
    assert(!aster_http_validate_headers(
        "Authorization: bearer x\r\nX-C: 1\r\n",
        strlen("Authorization: bearer x\r\nX-C: 1\r\n")));
    const char *invalid[] = {"Host: other\r\n",
                             "X: x\r\n\r\n",
                             "X: x\n",
                             "X: x\r\nInjected\r\n",
                             "Content-Length: 0\r\n",
                             "Accept-Encoding: gzip\r\n"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
        assert(aster_http_validate_headers(invalid[i], strlen(invalid[i])) < 0);
    /* Thousands of chunks do not count as an ever-growing header block. */
    struct aster_http_codec c;
    aster_http_codec_init(&c, false, 10000);
    size_t used, wrote;
    uint8_t out[1];
    const char *h = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    assert(!aster_http_codec_feed(&c, (void *)h, strlen(h), &used, out, 1,
                                  &wrote));
    for (int i = 0; i < 3000; ++i) {
        assert(!aster_http_codec_feed(&c, (void *)"1\r\nx\r\n", 6, &used, out,
                                      1, &wrote));
        assert(used == 6 && wrote == 1 && out[0] == 'x');
    }
    assert(!aster_http_codec_feed(&c, (void *)"0\r\n\r\n", 5, &used, out, 1,
                                  &wrote) &&
           c.state == HC_DONE && c.body_bytes == 3000);
    size_t len;
    char small[3] = {0};
    assert(aster_http_field_get(&c.headers, "Transfer-Encoding", 0, small, 1,
                                &len) == -EMSGSIZE);
    assert(len == 7 && small[0] == 0);
    puts("HTTP codec fragmentation, framing, trailers, URL and header "
         "boundaries "
         "OK");
}
