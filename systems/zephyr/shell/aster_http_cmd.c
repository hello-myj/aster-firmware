/* SPDX-License-Identifier: MIT
 * CLI submits work to the same bounded service; no blocking HTTP in shell.
 */
#include "aster_http_cmd.h"
#include "aster_http.h"
#include "aster_mem.h"
#include "aster_storage.h"
#if defined(CONFIG_ASTER_HTTP_TLS)
#include "aster_http_tls.h"
#endif
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>
#define OWNER ASTER_HTTP_CLI_OWNER
static int number(const char *s, uint32_t max, uint32_t *out)
{
    if (!s || !s[0] || s[0] == '-' || s[0] == '+')
        return -EINVAL;
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || *end || n > max)
        return -EINVAL;
    *out = n;
    return 0;
}
static int reply(const struct shell *sh, int rc)
{
    shell_print(sh, "HTTP result=%d", rc);
    return 0;
}
static int file_load(const char *path, char *out, size_t cap, size_t *length)
{
    struct fs_file_t f;
    fs_file_t_init(&f);
    int rc = fs_open(&f, path, FS_O_READ);
    if (rc)
        return rc;
    size_t used = 0;
    while (used < cap) {
        ssize_t n = aster_fs_read_safe(&f, out + used, cap - used);
        if (n < 0) {
            rc = n;
            break;
        }
        if (!n)
            break;
        used += n;
    }
    if (!rc && used == cap)
        rc = -EMSGSIZE;
    int closed = fs_close(&f);
    if (!rc)
        rc = closed;
    if (!rc) {
        out[used] = 0;
        *length = used;
    }
    return rc;
}
static int start(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 5 || argc > 7)
        return reply(sh, -EINVAL);
    struct aster_http_options o = {0};
    uint32_t limit;
    if (!strcmp(argv[1], "GET"))
        o.method = ASTER_HTTP_GET;
    else if (!strcmp(argv[1], "HEAD"))
        o.method = ASTER_HTTP_HEAD;
    else if (!strcmp(argv[1], "POST"))
        o.method = ASTER_HTTP_POST;
    else
        return reply(sh, -EINVAL);
    if (number(argv[3], 600000, &o.timeout_ms) ||
        number(argv[4], 67108864, &limit))
        return reply(sh, -EINVAL);
    o.url = argv[2];
    o.body_limit = limit;
    if (argc >= 6 && strcmp(argv[5], "-")) {
        o.body = argv[5];
        o.body_length = strlen(argv[5]);
    }
    char *headers = NULL;
    int rc = 0;
    if (argc == 7) {
        headers = aster_psram_alloc(ASTER_HTTP_HEADERS_MAX + 2);
        if (!headers)
            return reply(sh, -ENOMEM);
        rc = file_load(argv[6], headers, ASTER_HTTP_HEADERS_MAX + 1,
                       &o.headers_length);
        o.headers = headers;
    }
    if (!rc)
        rc = aster_http_svc_start(OWNER, &o);
    aster_psram_free(headers);
    if (rc < 0)
        return reply(sh, rc);
    shell_print(sh, "HTTP handle=%d queued=1", rc);
    return 0;
}
static int result(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    uint32_t h;
    struct aster_http_snapshot s;
    if (number(argv[1], INT32_MAX, &h))
        return reply(sh, -EINVAL);
    int rc = aster_http_svc_status(OWNER, h, &s);
    if (rc)
        return reply(sh, rc);
    shell_print(
        sh,
        "HTTP handle=%u state=%u status=%u error=%d tls_error=%d "
        "transport_error=%d verify=%08x redirects=%u queued=%u received=%llu "
        "length=%llu "
        "has_length=%u headers=%u",
        h, s.state, s.status, s.error, s.tls_error, s.transport_error,
        s.tls_verify, s.redirects, s.queued, (unsigned long long)s.received,
        (unsigned long long)s.content_length, s.has_length, s.headers_ready);
    return 0;
}
static int read_body(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    uint32_t h, cap;
    if (number(argv[1], INT32_MAX, &h) || number(argv[2], 1200, &cap) || !cap)
        return reply(sh, -EINVAL);
    uint8_t *out = aster_psram_alloc(cap);
    if (!out)
        return reply(sh, -ENOMEM);
    int rc = aster_http_svc_read(OWNER, h, out, cap);
    shell_print(sh, "HTTP handle=%u result=%d crc32=%08x", h, rc,
                rc > 0 ? crc32_ieee(out, rc) : 0);
    char encoded[257];
    const char digits[] = "0123456789abcdef";
    for (int offset = 0; offset < rc; offset += 128) {
        int n = rc - offset;
        if (n > 128)
            n = 128;
        for (int i = 0; i < n; ++i) {
            encoded[2 * i] = digits[out[offset + i] >> 4];
            encoded[2 * i + 1] = digits[out[offset + i] & 15];
        }
        encoded[n * 2] = 0;
        shell_print(sh, "HTTP data offset=%d hex=%s", offset, encoded);
    }
    aster_psram_free(out);
    return 0;
}
static int header(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    uint32_t h, index;
    size_t length = 0;
    char *out = aster_psram_alloc(ASTER_HTTP_HEADERS_MAX + 1);
    if (!out)
        return reply(sh, -ENOMEM);
    int rc = number(argv[1], INT32_MAX, &h) || number(argv[3], 32, &index)
                 ? -EINVAL
                 : aster_http_svc_header(OWNER, h, argv[2], index, out,
                                     ASTER_HTTP_HEADERS_MAX, &length);
    if (!rc) {
        out[length] = 0;
        shell_print(sh, "HTTP header_length=%u value=%s", (unsigned)length,
                    out);
    }
    aster_psram_free(out);
    return reply(sh, rc);
}
static int cancel(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    uint32_t h;
    return reply(sh, number(argv[1], INT32_MAX, &h)
                         ? -EINVAL
                         : aster_http_svc_cancel(OWNER, h));
}
static int close_request(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    uint32_t h;
    return reply(sh, number(argv[1], INT32_MAX, &h)
                         ? -EINVAL
                         : aster_http_svc_close(OWNER, h));
}
static int stats(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct aster_http_stats s;
    int rc = aster_http_svc_get_stats(&s);
    if (rc)
        return reply(sh, rc);
    shell_print(sh,
                "HTTP active=%u closing=%u tls=%u psram=%u stack=%u "
                "ca_bytes=%u cpu_yields=%u",
                s.active, s.closing, s.tls_active, s.psram_bytes,
                s.stack_bytes, s.ca_bytes, s.cpu_yields);
#if defined(CONFIG_ASTER_HTTP_TLS)
    size_t current, peak;
    aster_http_tls_heap(&current, &peak);
    shell_print(
        sh,
        "HTTP crypto_heap=%u crypto_current=%u crypto_peak=%u (global PSA/TLS)",
        CONFIG_MBEDTLS_HEAP_SIZE, (unsigned)current, (unsigned)peak);
#endif
    return 0;
}
static int ca(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc;
    if (!strcmp(argv[1], "default"))
        return reply(sh, aster_http_svc_ca_set(NULL, 0));
    char *pem = aster_psram_alloc(8193);
    if (!pem)
        return reply(sh, -ENOMEM);
    size_t length;
    int rc = file_load(argv[1], pem, 8192, &length);
    if (!rc)
        rc = aster_http_svc_ca_set(pem, length + 1);
    aster_psram_free(pem);
    return reply(sh, rc);
}
static const struct shell_static_entry http_entries[] = {
    SHELL_CMD_ARG(
        start, NULL,
        "METHOD URL TIMEOUT_MS BODY_LIMIT [TEXT_OR_DASH [HEADERS_FILE]]", start,
        5, 2),
    SHELL_CMD_ARG(result, NULL, "HANDLE", result, 2, 0),
    SHELL_CMD_ARG(read, NULL, "HANDLE MAX_BYTES (1..1200)", read_body, 3, 0),
    SHELL_CMD_ARG(header, NULL, "HANDLE NAME INDEX", header, 4, 0),
    SHELL_CMD_ARG(cancel, NULL, "HANDLE", cancel, 2, 0),
    SHELL_CMD_ARG(close, NULL, "HANDLE", close_request, 2, 0),
    SHELL_CMD(stats, NULL, "bounded HTTP and global crypto allocation", stats),
    SHELL_CMD_ARG(ca, NULL,
                  "default | ABSOLUTE_PEM_FILE (no active TLS requests)", ca, 2,
                  0),
    SHELL_SUBCMD_SET_END};
const union shell_cmd_entry aster_http_cmds = {.entry = http_entries};
