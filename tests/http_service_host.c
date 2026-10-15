/* SPDX-License-Identifier: MIT
 * Real HTTP service and real transport, stepped against POSIX TCP peers.
 */
#define ASTER_NET_HOST_TEST
#define ASTER_HTTP_HOST_TEST
#include "../systems/zephyr/services/aster_net.c"
#include "../systems/zephyr/services/aster_http.c"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
static int listener, remote = -1;
static char received_request[8192];
static size_t request_used;
static unsigned peers;
static void pump(void)
{
    ++host_now;
    service_tick();
    http_tick();
    if (remote < 0) {
        remote = accept(listener, NULL, NULL);
        if (remote >= 0) {
            fcntl(remote, F_SETFL, O_NONBLOCK);
            ++peers;
        }
    }
    if (remote >= 0 && request_used < sizeof(received_request) - 1) {
        int n = recv(remote, received_request + request_used,
                     sizeof(received_request) - 1 - request_used, 0);
        if (n > 0) {
            request_used += n;
            received_request[request_used] = 0;
        }
    }
}
static void ready(void)
{
    for (unsigned n = 0; n < 20000 && !strstr(received_request, "\r\n\r\n");
         ++n)
        pump();
    assert(strstr(received_request, "\r\n\r\n"));
}
static int start(enum aster_http_method method, const char *path,
                 const char *headers, const char *body, uint64_t limit)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    assert(!getsockname(listener, (void *)&a, &n));
    char url[200];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u%s", ntohs(a.sin_port),
             path);
    struct aster_http_options o = {.method = method,
                                   .url = url,
                                   .headers = headers,
                                   .headers_length =
                                       headers ? strlen(headers) : 0,
                                   .body = body,
                                   .body_length = body ? strlen(body) : 0,
                                   .timeout_ms = 30000,
                                   .body_limit = limit};
    return aster_http_svc_start(1, &o);
}
static struct aster_http_snapshot status(int h)
{
    struct aster_http_snapshot s;
    assert(!aster_http_svc_status(1, h, &s));
    return s;
}
static void dispose(int h)
{
    assert(!aster_http_svc_close(1, h));
    assert(aster_http_svc_status(1, h, &(struct aster_http_snapshot){0}) == -EBADF);
    for (unsigned n = 0; n < 5; ++n)
        pump();
    if (remote >= 0)
        close(remote);
    remote = -1;
    request_used = 0;
    received_request[0] = 0;
    assert(!psram_live);
}
static void send_wire(const char *s)
{
    size_t n = strlen(s);
    assert(send(remote, s, n, 0) == (ssize_t)n);
    for (unsigned i = 0; i < 20; ++i)
        pump();
}
static void content(void)
{
    int h = start(ASTER_HTTP_GET, "/hello", "X-Test: yes\r\n", NULL, 10000);
    assert(h > 0);
    ready();
    assert(strstr(received_request, "GET /hello HTTP/1.1\r\n"));
    assert(strstr(received_request, "Accept-Encoding: identity\r\n"));
    assert(strstr(received_request, "X-Test: yes\r\n"));
    send_wire(
        "HTTP/1.1 404 Missing\r\nTransfer-Encoding: chunked\r\nX-A: "
        "a\r\nX-A: b\r\n\r\n2\r\nhe\r\n3\r\nllo\r\n0\r\nX-T: done\r\n\r\n");
    assert(status(h).state == ASTER_HTTP_DONE && status(h).status == 404 &&
           status(h).received == 5);
    char b[100];
    size_t n;
    assert(!aster_http_svc_header(1, h, "X-A", 1, b, sizeof(b), &n) && n == 1 &&
           b[0] == 'b');
    assert(!aster_http_svc_header(1, h, "X-T", 0, b, sizeof(b), &n) && n == 4 &&
           !memcmp(b, "done", 4));
    assert(aster_http_svc_read(1, h, b, 2) == 2 && !memcmp(b, "he", 2));
    assert(aster_http_svc_read(1, h, b, 100) == 3 && !memcmp(b, "llo", 3));
    assert(!aster_http_svc_read(1, h, b, 100));
    assert(aster_http_svc_status(2, h, &(struct aster_http_snapshot){0}) == -EBADF);
    dispose(h);
}
static void streaming(void)
{
    int h = start(ASTER_HTTP_GET, "/big", NULL, NULL, 10000);
    ready();
    send_wire("HTTP/1.1 200 OK\r\nContent-Length: 8192\r\n\r\n");
    char b[8192];
    for (unsigned i = 0; i < sizeof(b); ++i)
        b[i] = i;
    assert(send(remote, b, sizeof(b), 0) == sizeof(b));
    for (unsigned i = 0; i < 100; ++i)
        pump();
    assert(status(h).queued == 2048 && status(h).state == ASTER_HTTP_BODY);
    for (unsigned i = 0; i < 50; ++i)
        pump();
    assert(status(h).queued == 2048);
    size_t total = 0;
    char out[137];
    for (unsigned i = 0; i < 20000 && total < sizeof(b); ++i) {
        pump();
        int n = aster_http_svc_read(1, h, out, sizeof(out));
        if (n == -EAGAIN)
            continue;
        assert(n > 0);
        assert(!memcmp(out, b + total, n));
        total += n;
    }
    assert(total == sizeof(b) && status(h).state == ASTER_HTTP_DONE);
    dispose(h);
}
static void failures(void)
{
    char b[20];
    int h = start(ASTER_HTTP_GET, "/truncated", NULL, NULL, 100);
    ready();
    send_wire("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nabc");
    close(remote);
    remote = -1;
    for (int i = 0; i < 30; ++i)
        pump();
    assert(status(h).error == -EPROTO);
    assert(aster_http_svc_read(1, h, b, sizeof(b)) == 3);
    assert(aster_http_svc_read(1, h, b, sizeof(b)) == -EPROTO);
    dispose(h);
    h = start(ASTER_HTTP_GET, "/cancel", NULL, NULL, 100);
    ready();
    assert(!aster_http_svc_cancel(1, h));
    pump();
    assert(status(h).state == ASTER_HTTP_CANCELLED);
    dispose(h);
    h = start(ASTER_HTTP_GET, "/timeout", NULL, NULL, 100);
    ready();
    host_now += 30001;
    pump();
    assert(status(h).error == -ETIMEDOUT);
    dispose(h);
    h = start(ASTER_HTTP_GET, "/limit", NULL, NULL, 2);
    ready();
    send_wire("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");
    assert(status(h).error == -EMSGSIZE);
    dispose(h);
}
static void early_response(void)
{
    char upload[2049];
    memset(upload, 'a', 2048);
    upload[2048] = 0;
    int h = start(ASTER_HTTP_POST, "/early", NULL, upload, 100);
    while (remote < 0)
        pump();
    send_blocked = 1;
    send_wire("HTTP/1.1 100 Continue\r\n\r\n");
    assert(status(h).state == ASTER_HTTP_SENDING);
    send_wire("HTTP/1.1 413 Too Large\r\nContent-Length: 5\r\n\r\nlarge");
    assert(status(h).state == ASTER_HTTP_DONE && status(h).status == 413 &&
           !status(h).error);
    char out[8];
    assert(aster_http_svc_read(1, h, out, sizeof(out)) == 5 &&
           !memcmp(out, "large", 5));
    send_blocked = 0;
    dispose(h);
    h = start(ASTER_HTTP_POST, "/cancel-tx", NULL, upload, 100);
    send_blocked = 1;
    for (unsigned i = 0; i < 50; ++i)
        pump();
    assert(status(h).state == ASTER_HTTP_SENDING);
    assert(!aster_http_svc_cancel(1, h));
    pump();
    assert(status(h).state == ASTER_HTTP_CANCELLED);
    assert(aster_http_svc_read(1, h, out, sizeof(out)) == -ECANCELED);
    send_blocked = 0;
    dispose(h);
    h = start(ASTER_HTTP_GET, "/cancel-connect", NULL, NULL, 100);
    assert(status(h).state == ASTER_HTTP_CONNECTING);
    assert(!aster_http_svc_cancel(1, h));
    pump();
    assert(status(h).state == ASTER_HTTP_CANCELLED);
    dispose(h);
}
static void redirect_test(void)
{
    int h =
        start(ASTER_HTTP_POST, "/start", "Authorization: x\r\n", "abc", 1000);
    ready();
    for (int i = 0; i < 10; ++i)
        pump();
    assert(strstr(received_request, "\r\n\r\nabc"));
    int other = aster_net_open(2, ASTER_NET_UDP, NULL, 0, 1000);
    assert(other > 0);
    send_wire(
        "HTTP/1.1 302 Found\r\nLocation: /next\r\nContent-Length: 0\r\n\r\n");
    close(remote);
    remote = -1;
    request_used = 0;
    received_request[0] = 0;
    ready();
    assert(strstr(received_request, "GET /next HTTP/1.1\r\n"));
    assert(strstr(received_request, "Authorization: x\r\n"));
    assert(strstr(received_request, "Content-Length: 0\r\n"));
    assert(aster_net_open(2, ASTER_NET_UDP, NULL, 0, 1000) == -ENOSPC);
    assert(status(h).redirects == 1);
    send_wire("HTTP/1.1 204 OK\r\n\r\n");
    assert(status(h).state == ASTER_HTTP_DONE);
    assert(!aster_net_close(2, other));
    dispose(h);
}
static void redirect_matrix(void)
{
    const unsigned codes[] = {301, 302, 303, 307, 308};
    for (unsigned i = 0; i < 5; ++i) {
        int h = start(ASTER_HTTP_POST, "/start",
                      "Authorization: secret\r\nCookie: a=b\r\n", "abc", 1000);
        ready();
        for (int j = 0; j < 10; ++j)
            pump();
        struct sockaddr_in a;
        socklen_t len = sizeof(a);
        assert(!getsockname(listener, (void *)&a, &len));
        char wire[256];
        snprintf(wire, sizeof(wire),
                 "HTTP/1.1 %u Found\r\nLocation: "
                 "http://localhost:%u/next\r\nContent-Length: 0\r\n\r\n",
                 codes[i], ntohs(a.sin_port));
        dns_mode = 1;
        send_wire(wire);
        close(remote);
        remote = -1;
        request_used = 0;
        received_request[0] = 0;
        ready();
        for (int j = 0; j < 10; ++j)
            pump();
        assert(!strstr(received_request, "Authorization:") &&
               !strstr(received_request, "Cookie:"));
        if (codes[i] >= 307) {
            assert(strstr(received_request, "POST /next"));
            assert(strstr(received_request, "\r\n\r\nabc"));
        } else {
            assert(strstr(received_request, "GET /next"));
            assert(strstr(received_request, "Content-Length: 0"));
        }
        send_wire("HTTP/1.1 204 OK\r\n\r\n");
        assert(status(h).state == ASTER_HTTP_DONE);
        dispose(h);
    }
    int h = start(ASTER_HTTP_GET, "/loop", NULL, NULL, 1000);
    ready();
    for (unsigned i = 0; i < 4; ++i) {
        send_wire("HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: "
                  "0\r\n\r\n");
        if (i == 3)
            break;
        close(remote);
        remote = -1;
        request_used = 0;
        received_request[0] = 0;
        ready();
    }
    assert(status(h).error == -ELOOP && status(h).redirects == 3);
    dispose(h);
    h = start(ASTER_HTTP_GET, "/cleanup", NULL, NULL, 1000);
    ready();
    assert(!aster_http_svc_close_owner(1));
    pump();
    pump();
    assert(aster_http_svc_status(1, h, &(struct aster_http_snapshot){0}) == -EBADF);
    assert(!aster_http_svc_close_owner(1));
    if (remote >= 0)
        close(remote);
    remote = -1;
    request_used = 0;
    received_request[0] = 0;
    assert(!psram_live);
}
static void dns_cleanup(void)
{
    /* Hold the actual transport's resolver callback pending. HTTP must own no
     * guest pointers and retire its private transport on all three paths. */
    for (unsigned mode = 0; mode < 3; ++mode) {
        dns_mode = 0;
        struct aster_http_options o = {.method=ASTER_HTTP_GET,
            .url="http://pending.test:12345/wait", .timeout_ms=100, .body_limit=1024};
        int h=aster_http_svc_start(1,&o);assert(h>0);
        pump();pump();assert(dns_active);
        if(mode==0){
            assert(!aster_http_svc_cancel(1,h));pump();pump();
            assert(status(h).state==ASTER_HTTP_CANCELLED);
            dispose(h);
        } else if(mode==1){
            assert(!aster_http_svc_close_owner(1));
            for(unsigned i=0;i<5;++i)pump();
            assert(aster_http_svc_status(1,h,&(struct aster_http_snapshot){0})==-EBADF);
            assert(!psram_live);
        } else {
            host_now+=101;pump();pump();
            assert(status(h).state==ASTER_HTTP_FAILED && status(h).error==-ETIMEDOUT);
            dispose(h);
        }
        assert(!dns_active);
    }
    dns_mode=1;
    int h=start(ASTER_HTTP_GET,"/after-dns",NULL,NULL,1000);
    ready();send_wire("HTTP/1.1 204 No Content\r\n\r\n");
    assert(status(h).state==ASTER_HTTP_DONE);dispose(h);
}
int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET,
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(listener, (void *)&a, sizeof(a)));
    assert(!listen(listener, 4));
    fcntl(listener, F_SETFL, O_NONBLOCK);
    content();
    streaming();
    failures();
    early_response();
    redirect_test();
    redirect_matrix();
    dns_cleanup();
    assert(!aster_http_svc_close_owner(1));
    assert(counters.opened == counters.closed);
    assert(!psram_live);
    free(http_stack);
    free(worker_stack);
    close(listener);
    puts("HTTP transport, streaming, isolation, redirects, cancellation and "
         "recovery OK");
}
