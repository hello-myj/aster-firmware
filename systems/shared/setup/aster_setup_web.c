/* SPDX-License-Identifier: MIT */
#include "aster_setup.h"
#include "aster_setup_json.h"
#include "aster_platform.h"
#include "aster_mem.h"
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define WEB_STACK 8192
extern const char aster_setup_page[];
static struct k_thread thread;
static k_thread_stack_t *stack;
static atomic_bool stop;
typedef struct {
    char request[4096], response[4096];
    aster_setup_json json;
    aster_setup_snapshot status;
    aster_wifi_ap_t networks[16];
} web_context;
static int ready(int fd, int timeout) {
    struct zsock_pollfd pfd = {.fd = fd, .events = ZSOCK_POLLIN};
    return zsock_poll(&pfd, 1, timeout) > 0 && (pfd.revents & ZSOCK_POLLIN);
}
static int send_all(int fd, const char *data, size_t length) {
    while (length && !atomic_load(&stop)) {
        struct zsock_pollfd pfd = {.fd = fd, .events = ZSOCK_POLLOUT};
        if (zsock_poll(&pfd, 1, 1000) <= 0)
            return -EIO;
        int n = zsock_send(fd, data, length, 0);
        if (n <= 0)
            return -EIO;
        data += n;
        length -= n;
    }
    return length ? -EIO : 0;
}
static void reply(int fd, int code, const char *type, const char *body) {
    char header[384];
    size_t length = strlen(body);
    int n = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d Result\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: "
        "close\r\nCache-Control: no-store\r\nX-Content-Type-Options: "
        "nosniff\r\nContent-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; "
        "style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'\r\n\r\n",
        code, type, (unsigned)length);
    if (n > 0 && n < sizeof(header) && !send_all(fd, header, n))
        send_all(fd, body, length);
}
static void api_status(int fd, web_context *c) {
    aster_setup_get(&c->status);
    aster_setup_snapshot *s = &c->status;
    char server[400];
    if (aster_setup_json_quote(server, sizeof(server), s->server) < 0)
        strcpy(server, "\"\"");
    snprintf(c->response, sizeof(c->response),
             "{\"phase\":%u,\"error\":%d,\"percent\":%u,\"bytes\":%llu,\"total\":%llu,\"seconds\":%"
             "u,\"token\":\"%s\",\"server\":%s}",
             s->state.phase, s->state.error, aster_setup_core_percent(&s->state),
             (unsigned long long)s->state.bytes, (unsigned long long)s->state.total,
             (unsigned)s->seconds, s->token, server);
    reply(fd, 200, "application/json; charset=utf-8", c->response);
}
static void api_networks(int fd, web_context *c) {
    int count = 0;
    if (aster_plat_wifi_scan_results(c->networks, 16, &count)) {
        reply(fd, 503, "application/json", "{}");
        return;
    }
    size_t n = 0;
    n += snprintf(c->response, sizeof(c->response), "{\"networks\":[");
    for (int i = 0; i < count && i < 16; i++) {
        char ssid[80];
        if (aster_setup_json_quote(ssid, sizeof(ssid), c->networks[i].ssid) < 0)
            continue;
        int w = snprintf(c->response + n, sizeof(c->response) - n, "%s{\"ssid\":%s,\"rssi\":%d}",
                         n > 13 ? "," : "", ssid, c->networks[i].rssi);
        if (w < 0 || (size_t)w >= sizeof(c->response) - n - 3)
            break;
        n += w;
    }
    snprintf(c->response + n, sizeof(c->response) - n, "]}");
    reply(fd, 200, "application/json; charset=utf-8", c->response);
}
static void handle(int fd, web_context *c) {
    size_t used = 0, header = 0, length = 0;
    char method[8], path[96], version[16];
    uint64_t deadline = k_uptime_get() + 5000;
    while (k_uptime_get() < deadline && !atomic_load(&stop)) {
        if (!ready(fd, 200))
            continue;
        int n = zsock_recv(fd, c->request + used, sizeof(c->request) - 1 - used, 0);
        if (n <= 0)
            return;
        used += n;
        c->request[used] = 0;
        if (!header) {
            char *end = strstr(c->request, "\r\n\r\n");
            if (end) {
                header = end - c->request + 4;
                if (sscanf(c->request, "%7s %95s %15s", method, path, version) != 3)
                    return;
                /* Deliberately one request/connection, no transfer coding or pipelining. */
                char *p = strstr(c->request, "\r\n");
                bool content_length = false;
                while (p && (size_t)(p - c->request) < header - 2) {
                    p += 2;
                    char *e = strstr(p, "\r\n");
                    if (!e || e == p)
                        break;
                    if (!strncasecmp(p, "Transfer-Encoding:", 18)) {
                        reply(fd, 400, "application/json", "{}");
                        return;
                    }
                    if (!strncasecmp(p, "Content-Length:", 15)) {
                        char *finish;
                        unsigned long value = strtoul(p + 15, &finish, 10);
                        if (content_length || finish != e || value > 1024) {
                            reply(fd, 400, "application/json", "{}");
                            return;
                        }
                        length = value;
                        content_length = true;
                    }
                    p = e;
                }
            }
        }
        if (header && used >= header + length)
            break;
        if (used == sizeof(c->request) - 1) {
            reply(fd, 413, "application/json", "{}");
            return;
        }
    }
    if (!header || used != header + length || memchr(c->request, 0, used))
        return;
    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/api/status")) {
            api_status(fd, c);
            return;
        }
        if (!strcmp(path, "/api/networks")) {
            api_networks(fd, c);
            return;
        }
        /* Captive-portal probe URLs show the same local page, no external redirect. */
        reply(fd, 200, "text/html; charset=utf-8", aster_setup_page);
        return;
    }
    if (strcmp(method, "POST") || aster_setup_json_parse(&c->json, c->request + header)) {
        reply(fd, 400, "application/json", "{}");
        return;
    }
    aster_setup_get(&c->status);
    const aster_setup_json_field *token = aster_setup_json_get(&c->json, "token");
    if (!token || token->is_number || strcmp(token->text, c->status.token)) {
        reply(fd, 403, "application/json", "{}");
        return;
    }
    int rc = -EINVAL;
    if (!strcmp(path, "/api/scan"))
        rc = aster_plat_wifi_scan_start();
    else if (!strcmp(path, "/api/retry")) {
        aster_setup_retry();
        rc = 0;
    } else if (!strcmp(path, "/api/connect")) {
        aster_setup_credentials_t credentials = {0};
        const char *keys[] = {"ssid", "password", "server"};
        char *dst[] = {credentials.ssid, credentials.password, credentials.server};
        size_t caps[] = {sizeof(credentials.ssid), sizeof(credentials.password),
                         sizeof(credentials.server)};
        bool valid = true;
        for (unsigned i = 0; i < 3; i++) {
            const aster_setup_json_field *f = aster_setup_json_get(&c->json, keys[i]);
            if (!f || f->is_number || strlen(f->text) >= caps[i]) {
                valid = false;
                break;
            }
            strcpy(dst[i], f->text);
        }
        if (valid)
            rc = aster_setup_submit(&credentials);
        memset(&credentials, 0, sizeof(credentials));
    }
    memset(c->request, 0, sizeof(c->request));
    memset(&c->json, 0, sizeof(c->json));
    reply(fd, rc ? 409 : 200, "application/json", rc ? "{\"accepted\":0}" : "{\"accepted\":1}");
}
static void run(void *a, void *b, void *d) {
    (void)a;
    (void)b;
    (void)d;
    web_context *c = aster_psram_alloc(sizeof(*c));
    if (!c)
        return;
    int listener = -1;
    while (!atomic_load(&stop)) {
        if (listener < 0) {
            listener = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener < 0) {
                k_msleep(200);
                continue;
            }
            struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(80)};
            zsock_inet_pton(AF_INET, "192.168.4.1", &address.sin_addr);
            if (zsock_bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
                zsock_listen(listener, 2)) {
                zsock_close(listener);
                listener = -1;
                k_msleep(500);
                continue;
            }
        }
        if (!ready(listener, 200))
            continue;
        int client = zsock_accept(listener, NULL, NULL);
        if (client < 0)
            continue;
        handle(client, c);
        zsock_close(client);
    }
    if (listener >= 0)
        zsock_close(listener);
    memset(c, 0, sizeof(*c));
    aster_psram_free(c);
}
int aster_setup_web_start(void) {
    if (stack)
        return 0;
    atomic_store(&stop, false);
    stack = aster_dram_alloc(K_KERNEL_STACK_LEN(WEB_STACK));
    if (!stack)
        return -ENOMEM;
    k_thread_create(&thread, stack, WEB_STACK, run, NULL, NULL, NULL, K_PRIO_PREEMPT(9), 0,
                    K_NO_WAIT);
    k_thread_name_set(&thread, "aster_provision");
    return 0;
}
bool aster_setup_web_stop(void) {
    if (!stack)
        return true;
    atomic_store(&stop, true);
    if (k_thread_join(&thread, K_NO_WAIT))
        return false;
    aster_dram_free(stack);
    stack = NULL;
    return true;
}
