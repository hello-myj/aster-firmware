/* SPDX-License-Identifier: MIT
 * CLI uses the same quotas as future APP clients; secrets only in a profile.
 */
#include "aster_mqtt_cmd.h"
#include "aster_mqtt.h"
#include "aster_mem.h"
#include "aster_storage.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>
#define OWNER ASTER_MQTT_CLI_OWNER
static int mqtt_number(const char *s, uint32_t max, uint32_t *out)
{
    if (!s || !*s || *s == '-' || *s == '+') return -EINVAL;
    char *end; errno = 0; unsigned long v = strtoul(s, &end, 10);
    if (errno || *end || v > max) return -EINVAL;
    *out = v; return 0;
}
static int mqtt_reply(const struct shell *sh, int rc)
{
    shell_print(sh, "MQTT result=%d", rc); return 0;
}
static int mqtt_hex(const char *s, uint8_t *out, size_t cap)
{
    if (!strcmp(s, "-")) return 0;
    size_t n = strlen(s);
    if (n % 2 || n / 2 > cap) return -EMSGSIZE;
    for (size_t i = 0; i < n / 2; ++i) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; ++j) {
            unsigned char c = s[2 * i + j];
            unsigned x = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (x > 15) return -EINVAL;
            v = (v << 4) | x;
        }
        out[i] = v;
    }
    return n / 2;
}
/* Bounded key=value profile; strict keys/duplicates, credentials never echoed. */
static int mqtt_connect_cmd(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    char *profile = aster_psram_alloc(2049);
    if (!profile) return mqtt_reply(sh, -ENOMEM);
    struct fs_file_t file; fs_file_t_init(&file);
    int rc = fs_open(&file, argv[1], FS_O_READ);
    size_t used = 0;
    if (!rc) {
        for (;;) {
            ssize_t n = aster_fs_read_safe(&file, profile + used, 2049 - used);
            if (n < 0) { rc = n; break; }
            if (!n) break;
            used += n;
            if (used > 2048) { rc = -EMSGSIZE; break; }
        }
        int closed = fs_close(&file); if (!rc) rc = closed;
    }
    if (rc) goto done;
    if (memchr(profile, 0, used)) { rc = -EINVAL; goto done; }
    profile[used] = 0;
    struct aster_mqtt_connect o = {.port = 1883, .timeout_ms = 15000,
        .keepalive_s = 15, .reconnect = true};
    uint8_t will[256]; uint32_t seen = 0;
    static const char *const keys[] = {"host", "client_id", "port", "tls",
        "reconnect", "keepalive_s", "timeout_ms", "username", "password",
        "will_topic", "will_hex", "will_qos", "will_retain"};
    char *cursor = profile;
    while (*cursor) {
        char *line = cursor, *end = strchr(cursor, '\n');
        if (end) { *end = 0; cursor = end + 1; } else cursor += strlen(cursor);
        size_t n = strlen(line); if (n && line[n - 1] == '\r') line[n - 1] = 0;
        if (!*line || *line == '#') continue;
        char *value = strchr(line, '=');
        if (!value) { rc = -EINVAL; goto done; }
        *value++ = 0; unsigned key;
        for (key = 0; key < sizeof(keys) / sizeof(keys[0]); ++key)
            if (!strcmp(line, keys[key])) break;
        if (key == sizeof(keys) / sizeof(keys[0]) || (seen & (1u << key))) { rc = -EINVAL; goto done; }
        seen |= 1u << key;
        uint32_t v = 0;
        if ((key >= 2 && key <= 6) || key >= 11) {
            if (mqtt_number(value, 65535, &v)) { rc = -EINVAL; goto done; }
        }
        switch (key) {
        case 0: o.host = value; break;
        case 1: o.client_id = value; break;
        case 2: o.port = v; break;
        case 3: if (v > 1) { rc = -EINVAL; goto done; } o.tls = v; break;
        case 4: if (v > 1) { rc = -EINVAL; goto done; } o.reconnect = v; break;
        case 5: o.keepalive_s = v; break;
        case 6: o.timeout_ms = v; break;
        case 7: o.username = value; break;
        case 8: o.password = value; o.password_length = strlen(value); break;
        case 9: o.will_topic = value; break;
        case 10: rc = mqtt_hex(value, will, sizeof(will));
            if (rc < 0) goto done;
            o.will_payload = will; o.will_length = rc; break;
        case 11: o.will_qos = v; break;
        case 12: if (v > 1) { rc = -EINVAL; goto done; } o.will_retain = v; break;
        }
    }
    rc = aster_mqtt_svc_open(OWNER, &o);
done:
    memset(profile, 0, 2049); aster_psram_free(profile);
    if (rc > 0) shell_print(sh, "MQTT handle=%d", rc);
    else mqtt_reply(sh, rc);
    return 0;
}
static int mqtt_status_cmd(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; uint32_t h;
    if (mqtt_number(argv[1], INT32_MAX, &h) || !h) return mqtt_reply(sh, -EINVAL);
    struct aster_mqtt_snapshot s;
    int rc = aster_mqtt_svc_status(OWNER, h, &s);
    if (rc) return mqtt_reply(sh, rc);
    shell_print(sh, "MQTT handle=%u state=%u error=%d connack=%u reconnects=%u queued=%u pending=%u subscriptions=%u received=%u sent=%u last_operation=%u operation_kind=%u operation_error=%d tls_verify=%u",
        h, s.state, s.error, s.connack, s.reconnects, s.queued, s.pending,
        s.subscriptions, s.received, s.sent, s.last_operation,
        s.last_operation_kind, s.operation_error, s.tls_verify);
    return 0;
}
static int mqtt_operation_cmd(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; uint32_t h, sequence; int32_t error;
    if (mqtt_number(argv[1], INT32_MAX, &h) || !h ||
        mqtt_number(argv[2], INT32_MAX, &sequence) || !sequence) return mqtt_reply(sh, -EINVAL);
    int rc = aster_mqtt_svc_operation(OWNER, h, sequence, &error);
    if (rc) return mqtt_reply(sh, rc);
    shell_print(sh, "MQTT operation=%u error=%d", sequence, error); return 0;
}
static int mqtt_publish_cmd(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; uint32_t h, qos, retain; uint8_t data[96];
    if (mqtt_number(argv[1], INT32_MAX, &h) || !h ||
        mqtt_number(argv[3], 1, &qos) || mqtt_number(argv[4], 1, &retain)) return mqtt_reply(sh, -EINVAL);
    int n = mqtt_hex(argv[5], data, sizeof(data));
    if (n < 0) return mqtt_reply(sh, n);
    int rc = aster_mqtt_svc_publish(OWNER, h, argv[2], data, n, qos, retain);
    if (rc > 0) shell_print(sh, "MQTT operation=%d", rc); else mqtt_reply(sh, rc);
    return 0;
}
static int mqtt_sub_cmd(const struct shell *sh, size_t argc, char **argv)
{
    uint32_t h, qos = 0;
    if (mqtt_number(argv[1], INT32_MAX, &h) || !h ||
        (argc == 4 && mqtt_number(argv[3], 1, &qos))) return mqtt_reply(sh, -EINVAL);
    int rc = aster_mqtt_svc_subscribe(OWNER, h, argv[2], qos, !strcmp(argv[0], "unsubscribe"));
    if (rc > 0) shell_print(sh, "MQTT operation=%d", rc); else mqtt_reply(sh, rc);
    return 0;
}
static int mqtt_recv_cmd(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; uint32_t h;
    if (mqtt_number(argv[1], INT32_MAX, &h) || !h) return mqtt_reply(sh, -EINVAL);
    struct aster_mqtt_message *m = aster_psram_alloc(sizeof(*m));
    if (!m) return mqtt_reply(sh, -ENOMEM);
    int rc = aster_mqtt_svc_recv(OWNER, h, m);
    if (!rc) {
        shell_print(sh, "MQTT message topic=%s length=%u qos=%u retain=%u dup=%u crc32=%08x",
            m->topic, m->length, m->qos, m->retain, m->dup, crc32_ieee(m->data, m->length));
        for (size_t off = 0; off < m->length; off += 64) {
            char hex[129]; size_t n = m->length - off;
            if (n > 64) n = 64;
            static const char digits[] = "0123456789abcdef";
            for (size_t i = 0; i < n; ++i) {
                hex[2 * i] = digits[m->data[off + i] >> 4];
                hex[2 * i + 1] = digits[m->data[off + i] & 15];
            }
            hex[2 * n] = 0; shell_print(sh, "MQTT data offset=%u hex=%s", (unsigned)off, hex);
        }
    } else mqtt_reply(sh, rc);
    aster_psram_free(m); return 0;
}
static int mqtt_end_cmd(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; uint32_t h;
    if (mqtt_number(argv[1], INT32_MAX, &h) || !h) return mqtt_reply(sh, -EINVAL);
    return mqtt_reply(sh, !strcmp(argv[0], "cancel") ?
        aster_mqtt_svc_cancel(OWNER, h) : aster_mqtt_svc_close(OWNER, h));
}
static int mqtt_stats_cmd(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; (void)argv; struct aster_mqtt_stats s;
    int rc = aster_mqtt_svc_stats(&s);
    if (rc) return mqtt_reply(sh, rc);
    shell_print(sh, "MQTT active=%u closing=%u tls=%u psram=%u", s.active, s.closing, s.tls_active, s.psram_bytes);
    return 0;
}
static const struct shell_static_entry mqtt_entries[] = {
    SHELL_CMD_ARG(connect, NULL, "PROFILE_PATH (bounded key=value; secrets not in CLI)", mqtt_connect_cmd, 2, 0),
    SHELL_CMD_ARG(result, NULL, "HANDLE", mqtt_status_cmd, 2, 0),
    SHELL_CMD_ARG(operation, NULL, "HANDLE SEQUENCE", mqtt_operation_cmd, 3, 0),
    SHELL_CMD_ARG(publish, NULL, "HANDLE TOPIC QOS RETAIN HEX (- empty, max96B)", mqtt_publish_cmd, 6, 0),
    SHELL_CMD_ARG(subscribe, NULL, "HANDLE FILTER QOS", mqtt_sub_cmd, 4, 0),
    SHELL_CMD_ARG(unsubscribe, NULL, "HANDLE FILTER", mqtt_sub_cmd, 3, 0),
    SHELL_CMD_ARG(recv, NULL, "HANDLE", mqtt_recv_cmd, 2, 0),
    SHELL_CMD_ARG(cancel, NULL, "HANDLE", mqtt_end_cmd, 2, 0),
    SHELL_CMD_ARG(close, NULL, "HANDLE", mqtt_end_cmd, 2, 0),
    SHELL_CMD(stats, NULL, "bounded MQTT clients", mqtt_stats_cmd),
    SHELL_SUBCMD_SET_END
};
const union shell_cmd_entry aster_mqtt_cmds = {.entry = mqtt_entries};
