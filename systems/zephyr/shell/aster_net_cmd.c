/* SPDX-License-Identifier: MIT
 * CLI is a client of the shared service, not a second transport stack.
 */
#include "aster_net_cmd.h"
#if defined(CONFIG_ASTER_HTTP)
#include "aster_http_cmd.h"
#endif
#if defined(CONFIG_ASTER_MQTT)
#include "aster_mqtt_cmd.h"
#endif
#include "aster_net.h"
#if defined(CONFIG_ASTER_WEBSOCKET)
#include "aster_ws_cmd.h"
#endif
#if defined(CONFIG_ASTER_MDNS)
#include "aster_mdns_cmd.h"
#endif
#include "aster_mem.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/crc.h>
#define OWNER ASTER_NET_CLI_OWNER
#define CLI_BYTES 128
/* 253-byte host + 96-byte hex payload fits the 512-byte shell line. */
#define CLI_TX_BYTES 96

static int number(const char *s, uint32_t max, uint32_t *out)
{
    if (!s || !s[0] || s[0] == '-' || s[0] == '+') return -EINVAL;
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || *end || n > max) return -EINVAL;
    *out = n;
    return 0;
}
static int reply(const struct shell *sh, int rc)
{
    shell_print(sh, "NET result=%d", rc);
    return 0; /* Protocol errors are in the bounded prefixed record. */
}
static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hex(const char *s, uint8_t *data)
{
    if (!strcmp(s, "-")) return 0; /* Empty UDP payload */
    size_t n = strlen(s);
    if (n % 2 || n > CLI_TX_BYTES * 2) return -EINVAL;
    for (size_t i = 0; i < n; i += 2) {
        int a = nibble(s[i]), b = nibble(s[i + 1]);
        if (a < 0 || b < 0) return -EINVAL;
        data[i / 2] = (a << 4) | b;
    }
    return n / 2;
}
static void interface_status(struct net_if *iface, void *user)
{
    const struct shell *sh = user;
    char ip[NET_IPV4_ADDR_LEN] = "-";
    bool up = net_if_is_up(iface), carrier = net_if_is_carrier_ok(iface);
    const struct in_addr *a = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (a) zsock_inet_ntop(AF_INET, a, ip, sizeof(ip));
    shell_print(sh, "NET interface=%d default=%d up=%d carrier=%d state=%s ipv4=%s",
                net_if_get_by_iface(iface), iface == net_if_get_default(), up, carrier,
                !up || !carrier ? "no-link" : a ? "address" : "await-ip", ip);
}
static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    shell_print(sh, "NET slots=%d tcp_buffer=%d udp_max=%d udp_depth=%d",
                ASTER_NET_SLOTS, ASTER_NET_TCP_BUFFER, ASTER_NET_UDP_MAX, ASTER_NET_UDP_DEPTH);
    net_if_foreach(interface_status, (void *)sh);
    return 0;
}
static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    struct aster_net_stats s;
    int rc = aster_net_get_stats(&s);
    if (rc) return reply(sh, rc);
    shell_print(sh, "NET active=%u closing=%u psram=%u stack=%u opened=%u closed=%u failed=%u cancelled=%u dropped=%u",
                s.active, s.closing, s.psram_bytes, s.stack_bytes, s.opened,
                s.closed, s.failed, s.cancelled, s.dropped);
    return 0;
}
static int cmd_connect(const struct shell *sh, size_t argc, char **argv)
{
    uint32_t port, timeout = 5000;
    if ((argc != 5 && argc != 7) || strcmp(argv[1], "--host") ||
        strcmp(argv[3], "--port") || number(argv[4], 65535, &port) || !port ||
        (argc == 7 && (strcmp(argv[5], "--timeout-ms") ||
                       number(argv[6], 30000, &timeout) || !timeout))) return reply(sh, -EINVAL);
    int h = aster_net_open(OWNER, ASTER_NET_TCP, argv[2], port, timeout);
    if (h < 0) return reply(sh, h);
    shell_print(sh, "NET handle=%d queued=1", h); return 0;
}
static int cmd_open(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    int h = aster_net_open(OWNER, ASTER_NET_UDP, NULL, 0, 5000);
    if (h < 0) return reply(sh, h);
    shell_print(sh, "NET handle=%d queued=1", h); return 0;
}
static int send_common(const struct shell *sh, size_t argc, char **argv, bool udp)
{
    uint32_t h, port = 0;
    uint8_t bytes[CLI_TX_BYTES];
    if (number(argv[1], INT32_MAX, &h) || !h) return reply(sh, -EINVAL);
    if ((!udp && (argc != 4 || strcmp(argv[2], "--hex"))) ||
        (udp && (argc != 8 || strcmp(argv[2], "--host") ||
         strcmp(argv[4], "--port") || number(argv[5], 65535, &port) ||
         !port || strcmp(argv[6], "--hex")))) return reply(sh, -EINVAL);
    int len = hex(argv[udp ? 7 : 3], bytes);
    if (len < 0) return reply(sh, len);
    return reply(sh, aster_net_send(OWNER, h, bytes, len, udp ? argv[3] : NULL, port));
}
static int cmd_send(const struct shell *s, size_t n, char **a) { return send_common(s, n, a, false); }
static int cmd_sendto(const struct shell *s, size_t n, char **a) { return send_common(s, n, a, true); }
static int cmd_recv(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    uint32_t h, cap, ip = 0;
    uint16_t port = 0;
    if (number(argv[1], INT32_MAX, &h) || !h || strcmp(argv[2], "--max-bytes") ||
        number(argv[3], ASTER_NET_UDP_MAX, &cap)) return reply(sh, -EINVAL);
    uint8_t *data = aster_psram_alloc(cap ? cap : 1);
    if (!data) return reply(sh, -ENOMEM);
    char encoded[CLI_BYTES * 2 + 1], addr[NET_IPV4_ADDR_LEN] = "-";
    const char digits[] = "0123456789abcdef";
    size_t required;
    int rc = aster_net_recv(OWNER, h, data, cap, &required, &ip, &port);
    if (ip) zsock_inet_ntop(AF_INET, &ip, addr, sizeof(addr));
    shell_print(sh, "NET handle=%u result=%d required=%u peer=%s:%u crc32=%08x",
                h, rc, (unsigned)required, addr, port,
                rc >= 0 ? crc32_ieee(data, rc) : 0);
    for (int offset = 0; offset < rc; offset += CLI_BYTES) {
        int count = rc - offset < CLI_BYTES ? rc - offset : CLI_BYTES;
        for (int i = 0; i < count; ++i) {
            encoded[2*i] = digits[data[offset+i] >> 4];
            encoded[2*i+1] = digits[data[offset+i] & 15];
        }
        encoded[count*2] = 0;
        shell_print(sh, "NET data offset=%d hex=%s", offset, encoded);
    }
    aster_psram_free(data);
    return 0;
}
static int cmd_result(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    uint32_t h;
    struct aster_net_snapshot s;
    if (number(argv[1], INT32_MAX, &h) || !h) return reply(sh, -EINVAL);
    int rc = aster_net_snapshot(OWNER, h, &s);
    if (rc) return reply(sh, rc);
    static const char *const states[] = { "invalid", "opening", "resolving", "connecting",
                                         "ready", "eof", "failed", "cancelled" };
    shell_print(sh, "NET handle=%u kind=%u state=%s phase=%u error=%d tx_queued=%u rx_queued=%u tx=%llu rx=%llu dropped=%u local_port=%u",
                h, s.kind, states[s.state], s.phase, s.error, s.tx_queued, s.rx_queued,
                (unsigned long long)s.tx_bytes, (unsigned long long)s.rx_bytes, s.dropped, s.local_port);
    return 0;
}
static int cmd_cancel(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; uint32_t h;
    if (number(argv[1], INT32_MAX, &h) || !h) return reply(sh, -EINVAL);
    return reply(sh, aster_net_cancel(OWNER, h));
}
static int cmd_close(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; uint32_t h;
    if (number(argv[1], INT32_MAX, &h) || !h) return reply(sh, -EINVAL);
    return reply(sh, aster_net_close(OWNER, h));
}
SHELL_STATIC_SUBCMD_SET_CREATE(tcp_cmds,
    SHELL_CMD_ARG(connect, NULL, "--host HOST --port PORT [--timeout-ms MS]", cmd_connect, 5, 2),
    SHELL_CMD_ARG(send, NULL, "HANDLE --hex HEX (max 96 bytes)", cmd_send, 4, 0),
    SHELL_CMD_ARG(recv, NULL, "HANDLE --max-bytes N (0..1200)", cmd_recv, 4, 0),
    SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(udp_cmds,
    SHELL_CMD_ARG(open, NULL, "ephemeral IPv4 endpoint", cmd_open, 1, 0),
    SHELL_CMD_ARG(sendto, NULL, "HANDLE --host HOST --port PORT --hex HEX (- means empty)", cmd_sendto, 8, 0),
    SHELL_CMD_ARG(recvfrom, NULL, "HANDLE --max-bytes N (0..1200)", cmd_recv, 4, 0),
    SHELL_SUBCMD_SET_END);
static const struct shell_static_entry net_entries[] = {
#if defined(CONFIG_ASTER_MDNS)
    SHELL_CMD(mdns, &aster_mdns_cmds, "bounded mDNS publisher", NULL),
#endif
#if defined(CONFIG_ASTER_WEBSOCKET)
    SHELL_CMD(ws, &aster_ws_cmds, "bounded WebSocket/WSS", NULL),
#endif
#if defined(CONFIG_ASTER_MQTT)
    SHELL_CMD(mqtt, &aster_mqtt_cmds, "bounded MQTT/MQTTS", NULL),
#endif
#if defined(CONFIG_ASTER_HTTP)
    SHELL_CMD(http, &aster_http_cmds, "bounded HTTP/HTTPS requests", NULL),
#endif
    SHELL_CMD(status, NULL, "link/address and limits; not Internet reachability", cmd_status),
    SHELL_CMD(stats, NULL, "shared service counters", cmd_stats),
    SHELL_CMD(tcp, &tcp_cmds, "async TCP", NULL),
    SHELL_CMD(udp, &udp_cmds, "async UDP", NULL),
    SHELL_CMD_ARG(result, NULL, "HANDLE", cmd_result, 2, 0),
    SHELL_CMD_ARG(cancel, NULL, "HANDLE", cmd_cancel, 2, 0),
    SHELL_CMD_ARG(close, NULL, "HANDLE", cmd_close, 2, 0),
    SHELL_SUBCMD_SET_END
};
const union shell_cmd_entry aster_net_cmds = { .entry = net_entries };
