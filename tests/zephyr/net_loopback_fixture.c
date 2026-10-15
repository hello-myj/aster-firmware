/* SPDX-License-Identifier: MIT
 * Optional device fixture, NOT a shipped server/API. Loopback only, two peers,
 * bounded echo; close after 512 bytes exercises the service's EOF handling.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/fcntl.h>

struct echo_peer {
    int fd;
    unsigned len, total;
    uint8_t data[128];
};
static struct echo_peer peers[2];
static struct k_thread fixture_thread;
K_THREAD_STACK_DEFINE(fixture_stack, 3072);
static void echo_run(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    int listener = zsock_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(9000) };
    zsock_inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (listener < 0) return;
    if (zsock_bind(listener, (struct sockaddr *)&addr, sizeof(addr)) ||
        zsock_listen(listener, 2) || zsock_fcntl(listener, F_SETFL, O_NONBLOCK)) {
        zsock_close(listener); return;
    }
    for (int i = 0; i < 2; ++i) peers[i].fd = -1;
    for (;;) {
        for (int i = 0; i < 2; ++i) {
            struct echo_peer *p = &peers[i];
            if (p->fd < 0) {
                int fd = zsock_accept(listener, NULL, NULL);
                if (fd >= 0) {
                    if (zsock_fcntl(fd, F_SETFL, O_NONBLOCK)) { zsock_close(fd); continue; }
                    *p = (struct echo_peer){ .fd = fd };
                }
                continue;
            }
            if (p->len) {
                int n = zsock_send(p->fd, p->data, p->len, MSG_DONTWAIT);
                if (n > 0) { p->len -= n; memmove(p->data, p->data+n, p->len); }
                else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) goto close_peer;
                if (p->len) continue;
            }
            if (p->total == 512) goto close_peer;
            unsigned cap = MIN(sizeof(p->data), 512 - p->total);
            int n = zsock_recv(p->fd, p->data, cap, MSG_DONTWAIT);
            if (n > 0) { p->len = n; p->total += n; }
            else if (!n || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
close_peer:
                zsock_close(p->fd); p->fd = -1;
            }
        }
        k_msleep(10);
    }
}
static int fixture_init(void)
{
    k_thread_create(&fixture_thread, fixture_stack, K_THREAD_STACK_SIZEOF(fixture_stack),
                    echo_run, NULL, NULL, NULL, K_PRIO_PREEMPT(11), 0, K_NO_WAIT);
    k_thread_name_set(&fixture_thread, "net_fixture");
    return 0;
}
SYS_INIT(fixture_init, APPLICATION, 99);
