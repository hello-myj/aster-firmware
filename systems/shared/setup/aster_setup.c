/* SPDX-License-Identifier: MIT
 * OS-neutral orchestration over the existing platform, filesystem, HTTP and
 * task adapters. No panel, radio pin or CPU-specific decisions live here.
 * Only the setup worker writes resources. UI and Web publish commands.
 */
#include "aster_setup.h"
#include "aster_setup_json.h"
#include "aster_storage.h"
#include "aster_app_mgr.h"
#include "aster_kv.h"
#include "aster_http.h"
#include "aster_crypto.h"
#include "aster_sysinfo.h"
#include "aster_platform.h"
#include "aster_mem.h"
#include "aster_bytes.h"
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>
#include <psa/crypto.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
LOG_MODULE_REGISTER(aster_setup, LOG_LEVEL_INF);
#define OWNER UINT64_C(0x5345545550)
#define STACK 12288
#define BUFFER 16384
#define PARTS ASTER_DIR "/system/setup"
extern int aster_setup_web_start(void);
extern bool aster_setup_web_stop(void);
static K_MUTEX_DEFINE(lock);
static struct k_thread thread;
static k_thread_stack_t *stack;
static bool started, begin_command, retry_command, credentials_command;
static bool repair_command, forced_boot;
static bool format_command, format_boot;
static aster_setup_snapshot snapshot;
static aster_setup_credentials_t pending;
static uint64_t began;
static uint64_t done_since;
static const uint8_t sizes[] = {12, 14, 16, 18, 20, 24, 28, 32, 40, 48};
typedef struct {
    aster_setup_json json;
    char response[4096], url[512], server[192], release[65], market[192];
    char digest[65], relative[128], destination[160], part[160];
    uint8_t buffer[BUFFER];
    uint32_t files;
    uint64_t total, completed;
} context;
static void phase(aster_setup_phase_t next) {
    k_mutex_lock(&lock, K_FOREVER);
    aster_setup_core_step(&snapshot.state, next);
    k_mutex_unlock(&lock);
    LOG_INF("setup phase %d", next);
}
static void progress(uint64_t done, uint64_t total, unsigned file, const char *name) {
    k_mutex_lock(&lock, K_FOREVER);
    snapshot.state.bytes = done;
    snapshot.state.total = total;
    snapshot.state.file = file;
    if (name)
        snprintf(snapshot.file, sizeof(snapshot.file), "%.63s", name);
    k_mutex_unlock(&lock);
}
static bool connected(void) {
    aster_wifi_status_t status = {0};
    return !aster_plat_wifi_status(&status) && status.state == ASTER_PLAT_WIFI_STATE_COMPLETED &&
           status.ip[0] && strcmp(status.ip, "0.0.0.0");
}
void aster_setup_begin(void) {
    k_mutex_lock(&lock, K_FOREVER);
    if (snapshot.state.phase == ASTER_SETUP_WELCOME)
        begin_command = true;
    k_mutex_unlock(&lock);
}
void aster_setup_retry(void) {
    k_mutex_lock(&lock, K_FOREVER);
    if (snapshot.state.phase == ASTER_SETUP_ERROR || snapshot.state.phase == ASTER_SETUP_STORAGE)
        retry_command = true;
    k_mutex_unlock(&lock);
}
int aster_setup_repair(void) {
    if (aster_app_mgr_is_live(NULL))
        return -EBUSY;
    k_mutex_lock(&lock, K_FOREVER);
    int rc = started && snapshot.state.phase == ASTER_SETUP_DONE ? 0 : -EBUSY;
    if (!rc)
        repair_command = true;
    k_mutex_unlock(&lock);
    return rc;
}
int aster_setup_submit(const aster_setup_credentials_t *c) {
    if (!aster_setup_credentials_valid(c))
        return -EINVAL;
    k_mutex_lock(&lock, K_FOREVER);
    if (credentials_command || (snapshot.state.phase != ASTER_SETUP_PROVISION &&
                                snapshot.state.phase != ASTER_SETUP_ERROR)) {
        k_mutex_unlock(&lock);
        return -EBUSY;
    }
    pending = *c;
    credentials_command = true;
    k_mutex_unlock(&lock);
    LOG_INF("setup configuration queued");
    return 0;
}
int aster_setup_request_format(void) {
    if (!aster_storage_can_format())
        return -ENOTSUP;
    if (aster_app_mgr_is_live(NULL))
        return -EBUSY;
    k_mutex_lock(&lock, K_FOREVER);
    int rc = started && snapshot.state.phase == ASTER_SETUP_DONE && !snapshot.hotspot ? 0 : -EBUSY;
    if (!rc)
        format_command = true;
    k_mutex_unlock(&lock);
    return rc;
}
bool aster_setup_format_pending(void) {
    k_mutex_lock(&lock, K_FOREVER);
    bool pending = format_command;
    k_mutex_unlock(&lock);
    return pending;
}
void aster_setup_format_ui_ready(void) {
    k_mutex_lock(&lock, K_FOREVER);
    if (format_command) {
        format_command = false;
        format_boot = true;
        repair_command = true;
    }
    k_mutex_unlock(&lock);
}
static bool consume(bool *command) {
    k_mutex_lock(&lock, K_FOREVER);
    bool yes = *command;
    *command = false;
    k_mutex_unlock(&lock);
    return yes;
}
static bool take_credentials(aster_setup_credentials_t *out) {
    k_mutex_lock(&lock, K_FOREVER);
    bool available = credentials_command;
    if (available) {
        *out = pending;
        memset(&pending, 0, sizeof(pending));
        credentials_command = false;
        strcpy(snapshot.server, out->server);
        aster_setup_core_step(&snapshot.state, ASTER_SETUP_CONNECT);
        LOG_INF("setup configuration consumed");
    }
    k_mutex_unlock(&lock);
    return available;
}
static bool resources_complete(void) {
    if (!aster_storage_mounted())
        return false;
    for (unsigned i = 0; i < sizeof(sizes); i++) {
        char path[96];
        uint8_t header[64] = {0};
        struct fs_dirent st;
        struct fs_file_t f;
        snprintf(path, sizeof(path), ASTER_FONTS_DIR "/ui_%u.cpfont", sizes[i]);
        fs_file_t_init(&f);
        if (fs_stat(path, &st) || st.size < 96 || fs_open(&f, path, FS_O_READ))
            return false;
        int n = aster_fs_read_safe(&f, header, sizeof(header));
        uint64_t intervals = aster_le32(header + 36), glyphs = aster_le32(header + 40);
        uint64_t bitmap = 64 + 12 * intervals + 16 * glyphs;
        bool valid = n == sizeof(header) && !memcmp(header, "CPFONT\0\0", 8) &&
                     aster_le16(header + 8) == 4 && !aster_le16(header + 10) && header[12] == 1 &&
                     aster_le32(header + 56) == 64 && glyphs && glyphs <= 65536 &&
                     bitmap <= st.size;
        uint8_t last[16];
        valid = valid && !fs_seek(&f, bitmap - 16, FS_SEEK_SET) &&
                aster_fs_read_safe(&f, last, 16) == 16 &&
                bitmap + aster_le32(last + 12) + aster_le16(last + 8) == st.size;
        fs_close(&f);
        if (!valid)
            return false;
    }
    aster_app_rec_t *rec = aster_psram_alloc(sizeof(*rec));
    if (!rec)
        return false;
    bool ok = aster_app_mgr_find_snapshot("aster.store", rec) && rec->has_aot && !rec->aot_skip;
    aster_psram_free(rec);
    return ok;
}
static bool healthy(void) {
    uint8_t pending_byte;
    size_t length = 0;
    /* Presence (even a truncated marker) means setup did not commit. This
     * closes the power-loss window after Store activation but before its
     * server settings and final initialization state have been saved. */
    if (aster_kv_get("aster.setup", "pending", &pending_byte, sizeof(pending_byte), &length) !=
        -ENOENT)
        return false;
    return resources_complete();
}
static const char *str(context *c, const char *key) {
    const aster_setup_json_field *f = aster_setup_json_get(&c->json, key);
    return f && !f->is_number ? f->text : NULL;
}
static uint64_t num(context *c, const char *key) {
    const aster_setup_json_field *f = aster_setup_json_get(&c->json, key);
    return f && f->is_number ? f->number : UINT64_MAX;
}
static bool hex64(const char *p) {
    if (!p || strlen(p) != 64)
        return false;
    for (; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
            return false;
    return true;
}
static int headers(uint32_t h, struct aster_http_snapshot *s) {
    int rc;
    uint64_t start = k_uptime_get();
    do {
        rc = aster_http_svc_status(OWNER, h, s);
        if (rc && rc != -EAGAIN)
            return rc;
        if (!rc && s->headers_ready)
            return 0;
        if (!rc && s->error)
            return s->error;
        if (k_uptime_get() - start > 30000)
            return -ETIMEDOUT;
        k_msleep(10);
    } while (true);
}
static int start_request(const struct aster_http_options *options) {
    uint64_t until = k_uptime_get() + 5000;
    int handle;
    do {
        handle = aster_http_svc_start(OWNER, options);
        if (handle != -EAGAIN && handle != -EBUSY && handle != -ENOSPC)
            return handle;
        k_msleep(20);
    } while (k_uptime_get() < until);
    return handle;
}
static int close_request(uint32_t handle) {
    uint64_t until = k_uptime_get() + 5000;
    int rc;
    do {
        rc = aster_http_svc_close(OWNER, handle);
        if (rc != -EAGAIN)
            return rc;
        k_msleep(10);
    } while (k_uptime_get() < until);
    return rc;
}
static int range_header(uint32_t handle, char *value, size_t capacity, size_t *length) {
    uint64_t until = k_uptime_get() + 5000;
    int rc;
    do {
        rc = aster_http_svc_header(OWNER, handle, "content-range", 0, value, capacity, length);
        if (rc != -EAGAIN)
            return rc;
        k_msleep(10);
    } while (k_uptime_get() < until);
    return rc;
}
static int get_json(context *c) {
    struct aster_http_options options = {.method = ASTER_HTTP_GET,
                                         .url = c->url,
                                         .timeout_ms = 30000,
                                         .body_limit = sizeof(c->response) - 1};
    int h = start_request(&options);
    if (h < 0)
        return h;
    struct aster_http_snapshot s;
    int rc = headers(h, &s);
    size_t n = 0;
    if (!rc && s.status != 200)
        rc = -EPROTO;
    while (!rc) {
        int r = aster_http_svc_read(OWNER, h, c->response + n, sizeof(c->response) - 1 - n);
        if (r == -EAGAIN) {
            k_msleep(10);
            continue;
        }
        if (r < 0) {
            rc = r;
            break;
        }
        if (!r)
            break;
        n += (unsigned)r;
        if (n == sizeof(c->response) - 1) {
            rc = -EFBIG;
            break;
        }
    }
    int close = close_request(h);
    if (!rc)
        rc = close;
    c->response[n] = 0;
    return rc ? rc : aster_setup_json_parse(&c->json, c->response) ? -EPROTO : 0;
}
static int hash_file(context *c, const char *path, uint64_t expected, char output[65]) {
    int64_t began_ms = k_uptime_get();
    uint64_t read_ms = 0, hash_ms = 0, read_bytes = 0;
    struct fs_dirent st;
    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_stat(path, &st) || st.size != expected || fs_open(&f, path, FS_O_READ))
        return -ENOENT;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    k_mutex_lock(&aster_crypto_lock, K_FOREVER);
    psa_status_t status = psa_crypto_init();
    if (status == PSA_SUCCESS)
        status = psa_hash_setup(&op, PSA_ALG_SHA_256);
    k_mutex_unlock(&aster_crypto_lock);
    int rc = status == PSA_SUCCESS ? 0 : -EIO;
    while (!rc) {
        int64_t tick = k_uptime_get();
        int n = aster_fs_read_safe(&f, c->buffer, sizeof(c->buffer));
        read_ms += k_uptime_get() - tick;
        if (n < 0) {
            rc = n;
            break;
        }
        if (!n)
            break;
        read_bytes += (unsigned)n;
        tick = k_uptime_get();
        k_mutex_lock(&aster_crypto_lock, K_FOREVER);
        status = psa_hash_update(&op, c->buffer, n);
        k_mutex_unlock(&aster_crypto_lock);
        hash_ms += k_uptime_get() - tick;
        if (status != PSA_SUCCESS)
            rc = -EIO;
        k_msleep(1);
    }
    uint8_t digest[32];
    size_t length = 0;
    k_mutex_lock(&aster_crypto_lock, K_FOREVER);
    if (!rc && psa_hash_finish(&op, digest, sizeof(digest), &length) != PSA_SUCCESS)
        rc = -EIO;
    psa_hash_abort(&op);
    k_mutex_unlock(&aster_crypto_lock);
    fs_close(&f);
    if (!rc && length != 32)
        rc = -EIO;
    if (!rc) {
        for (unsigned i = 0; i < 32; i++)
            snprintf(output + 2 * i, 3, "%02x", digest[i]);
    }
    LOG_INF("setup hash bytes=%llu wall_ms=%llu sd_read_ms=%llu sha_ms=%llu result=%d",
            (unsigned long long)read_bytes, (unsigned long long)(k_uptime_get()-began_ms),
            (unsigned long long)read_ms, (unsigned long long)hash_ms, rc);
    return rc;
}
static int fetch(context *c, uint64_t bytes, unsigned index) {
    char hash[65];
    phase(ASTER_SETUP_VERIFY);
    if (!hash_file(c, c->destination, bytes, hash) && !strcmp(hash, c->digest))
        return 0;
    phase(ASTER_SETUP_DOWNLOAD);
    struct fs_dirent st;
    uint64_t offset = !fs_stat(c->part, &st) ? st.size : 0;
    if (offset > bytes) {
        int r = fs_unlink(c->part);
        if (r)
            return r;
        offset = 0;
    }
    struct fs_statvfs space;
    if (!fs_statvfs(ASTER_MOUNT_POINT, &space)) {
        uint64_t free_bytes = (uint64_t)space.f_frsize * space.f_bfree;
        uint64_t reserve = index == 11 ? bytes * 2 : 1024 * 1024;
        if (free_bytes < bytes - offset + reserve)
            return -ENOSPC;
    }
    if (offset < bytes) {
        char request[192];
        snprintf(request, sizeof(request), "Range: bytes=%llu-\r\nIf-Range: \"%s\"\r\n",
                 (unsigned long long)offset, c->digest);
        struct aster_http_options opts = {.method = ASTER_HTTP_GET,
                                          .url = c->url,
                                          .headers = offset ? request : NULL,
                                          .headers_length = offset ? strlen(request) : 0,
                                          .timeout_ms = 600000,
                                          .body_limit = bytes};
        int h = start_request(&opts);
        if (h < 0)
            return h;
        struct aster_http_snapshot s;
        int rc = headers(h, &s);
        if (!rc && s.status == 200)
            offset = 0;
        else if (!rc && s.status != 206)
            rc = -EPROTO;
        if (!rc && s.status == 206) {
            char value[128];
            size_t length;
            unsigned long long first, last, total;
            if (range_header(h, value, sizeof(value), &length) ||
                sscanf(value, "bytes %llu-%llu/%llu", &first, &last, &total) != 3 ||
                first != offset || last != bytes - 1 || total != bytes)
                rc = -EPROTO;
        }
        if (!rc && s.has_length && s.content_length != bytes - offset)
            rc = -EPROTO;
        struct fs_file_t f;
        fs_file_t_init(&f);
        bool opened = false;
        int64_t transfer_ms = k_uptime_get();
        uint64_t write_ms = 0, wait_ms = 0, received = 0;
        unsigned reads = 0, empty_reads = 0;
        size_t buffered = 0;
        if (!rc) {
            rc = fs_open(&f, c->part,
                         FS_O_WRITE | FS_O_CREATE | (offset ? FS_O_APPEND : FS_O_TRUNC));
            opened = !rc;
        }
        while (!rc) {
            /* The transport may publish one small packet at a time. Batch
             * those into the existing 16 KiB buffer: short FAT writes cause
             * repeated sector read/modify/write and cluster bookkeeping. */
            int n = aster_http_svc_read(OWNER, h, c->buffer + buffered,
                                       sizeof(c->buffer) - buffered);
            if (n == -EAGAIN) {
                int64_t tick = k_uptime_get();
                /* svc_read wakes the HTTP worker. Yield briefly so the
                 * lower-priority TCP worker can refill its bounded queue;
                 * a 10 ms wait throttles every small receive batch. */
                k_msleep(1);
                wait_ms += k_uptime_get() - tick;
                empty_reads++;
                continue;
            }
            if (n < 0) {
                rc = n;
                break;
            }
            if ((uint64_t)n > bytes - offset - buffered) {
                rc = -EFBIG;
                break;
            }
            buffered += (unsigned)n;
            received += (unsigned)n;
            if (n)
                reads++;
            if (n && buffered < sizeof(c->buffer))
                continue;
            if (!buffered)
                break;
            int64_t tick = k_uptime_get();
            int w = aster_fs_write_safe(&f, c->buffer, buffered);
            write_ms += k_uptime_get() - tick;
            if (w != (int)buffered) {
                rc = w < 0 ? w : -ENOSPC;
                break;
            }
            offset += buffered;
            buffered = 0;
            progress(c->completed + offset, c->total, index, c->relative);
            if (!n)
                break;
            k_msleep(1);
        }
        if (opened) {
            int64_t tick = k_uptime_get();
            int sync = fs_sync(&f);
            int close = fs_close(&f);
            write_ms += k_uptime_get() - tick;
            if (!rc)
                rc = sync ? sync : close;
        }
        int closed = close_request(h);
        LOG_INF("setup transfer bytes=%llu wall_ms=%llu sd_write_ms=%llu wait_ms=%llu reads=%u empty=%u result=%d",
                (unsigned long long)received, (unsigned long long)(k_uptime_get()-transfer_ms),
                (unsigned long long)write_ms, (unsigned long long)wait_ms, reads, empty_reads, rc ? rc : closed);
        if (!rc)
            rc = closed;
        if (rc)
            return rc;
        if (offset != bytes)
            return -EIO;
    }
    phase(ASTER_SETUP_VERIFY);
    int rc = hash_file(c, c->part, bytes, hash);
    if (rc || strcmp(hash, c->digest)) {
        fs_unlink(c->part);
        return rc ? rc : -EBADMSG;
    }
    /* FatFs replacement is not power-loss atomic; keep the verified staging
     * file until rename succeeds. A missing destination restarts repair. */
    if (!fs_stat(c->destination, &st)) {
        rc = fs_unlink(c->destination);
        if (rc)
            return rc;
    }
    rc = fs_rename(c->part, c->destination);
    phase(ASTER_SETUP_DOWNLOAD);
    return rc;
}
static int provision(void) {
    phase(ASTER_SETUP_PROVISION);
    aster_wifi_hotspot_status_t ap = {0};
    aster_plat_wifi_hotspot_status(&ap);
    if (!ap.enabled || strcmp(ap.ssid, snapshot.ssid)) {
        int rc = aster_plat_wifi_hotspot_start(snapshot.ssid, snapshot.password);
        if (rc)
            return rc;
        uint64_t until = k_uptime_get() + 20000;
        do {
            aster_plat_wifi_hotspot_status(&ap);
            if (ap.enabled && !strcmp(ap.ssid, snapshot.ssid))
                break;
            k_msleep(100);
        } while (k_uptime_get() < until);
        if (!ap.enabled || strcmp(ap.ssid, snapshot.ssid))
            return -ETIMEDOUT;
    }
    k_mutex_lock(&lock, K_FOREVER);
    snapshot.hotspot = true;
    k_mutex_unlock(&lock);
    return aster_setup_web_start();
}
static int join(aster_setup_credentials_t *credentials) {
    phase(ASTER_SETUP_CONNECT);
    aster_wifi_status_t status = {0};
    if (connected() && !aster_plat_wifi_status(&status) && !strcmp(status.ssid, credentials->ssid))
        return 0;
    int rc = aster_plat_wifi_connect_start(credentials->ssid, credentials->password);
    memset(credentials->password, 0, sizeof(credentials->password));
    if (rc)
        return rc;
    uint64_t end = k_uptime_get() + 40000;
    do {
        if (connected() && !aster_plat_wifi_status(&status) &&
            !strcmp(status.ssid, credentials->ssid))
            return 0;
        k_msleep(100);
    } while (k_uptime_get() < end);
    return -ETIMEDOUT;
}
static int download(context *c) {
    phase(ASTER_SETUP_PLAN);
    aster_system_info_t info;
    aster_sysinfo_get(&info);
    aster_display_info_t display;
    memset(&display, 0, sizeof(display));
    aster_plat_display_get_info(0, &display);
    snprintf(c->url, sizeof(c->url),
             "%s/v1/setup/resolve?board=%s&target=%s&abi=%u&width=%u&height=%u&features=%llu",
             c->server, info.board, aster_arch_name(info.cpu_arch), (unsigned)info.abi_version,
             (unsigned)display.width, (unsigned)display.height,
             (unsigned long long)info.feature_flags);
    int rc = get_json(c);
    if (rc)
        return rc;
    const char *release = str(c, "release"), *market = str(c, "market_url");
    uint64_t files = num(c, "files"), total = num(c, "total_bytes");
    if (num(c, "schema") != 1 || !hex64(release) || files != 11 || total > 256 * 1024 * 1024 ||
        !market || !aster_setup_server_url(market))
        return -EPROTO;
    strcpy(c->release, release);
    strcpy(c->market, market);
    c->files = files;
    c->total = total;
    c->completed = 0;
    k_mutex_lock(&lock, K_FOREVER);
    snapshot.state.files = files;
    k_mutex_unlock(&lock);
    if ((rc = aster_storage_mkdir_p(PARTS)))
        return rc;
    /* Download sequentially: the staging quota is one file plus the Store,
     * not a second complete 96 MiB font tree. */
    for (unsigned i = 0; i < c->files; i++) {
        phase(ASTER_SETUP_DOWNLOAD);
        snprintf(c->url, sizeof(c->url), "%s/v1/setup/releases/%s/files/%u", c->server, c->release,
                 i);
        if ((rc = get_json(c)))
            return rc;
        const char *path = str(c, "path"), *digest = str(c, "sha256"), *kind = str(c, "kind"),
                   *url = str(c, "url");
        uint64_t bytes = num(c, "bytes");
        char expected[64], expected_url[128];
        if (i < sizeof(sizes))
            snprintf(expected, sizeof(expected), "fonts/ui_%u.cpfont", sizes[i]);
        else
            strcpy(expected, "store/package.ast");
        snprintf(expected_url, sizeof(expected_url), "/v1/setup/releases/%s/payload/%u", c->release,
                 i);
        if (num(c, "schema") != 1 || num(c, "index") != i || !path || strcmp(path, expected) ||
            !digest || !hex64(digest) || !kind ||
            strcmp(kind, i < sizeof(sizes) ? "font" : "store") || !url ||
            strcmp(url, expected_url) || !bytes || bytes > 64 * 1024 * 1024)
            return -EPROTO;
        strcpy(c->relative, expected);
        strcpy(c->digest, digest);
        if (i < sizeof(sizes))
            snprintf(c->destination, sizeof(c->destination), ASTER_DIR "/%s", expected);
        else
            strcpy(c->destination, PARTS "/store.ast");
        snprintf(c->part, sizeof(c->part), PARTS "/%s.part", c->digest);
        snprintf(c->url, sizeof(c->url), "%s%s", c->server, expected_url);
        progress(c->completed, c->total, i + 1, path);
        if ((rc = fetch(c, bytes, i + 1)))
            return rc;
        c->completed += bytes;
        if (c->completed > c->total)
            return -EPROTO;
        progress(c->completed, c->total, i + 1, path);
    }
    if (c->completed != c->total)
        return -EPROTO;
    phase(ASTER_SETUP_INSTALL);
    if ((rc = aster_app_mgr_install(PARTS "/store.ast")))
        return rc;
    if (!resources_complete())
        return -ENOEXEC;
    if ((rc = aster_kv_set("aster.store", "store.url", c->market, strlen(c->market))))
        return rc;
    /* Save only the non-secret resource endpoint. Wi-Fi credentials belong
     * to the platform's existing profile persistence. */
    if ((rc = aster_kv_set("aster.setup", "server", c->server, strlen(c->server))))
        return rc;
    if ((rc = aster_kv_delete("aster.setup", "pending")))
        return rc;
    fs_unlink(PARTS "/store.ast");
    return 0;
}
static void worker(void *a, void *b, void *d) {
    (void)a;
    (void)b;
    (void)d;
    if (format_boot) {
        format_boot = false;
        phase(ASTER_SETUP_FORMAT);
        /* UI has hidden the launcher and closed all font handles. Give its
         * flash-font status page one frame before formatting/resetting. */
        k_msleep(200);
        for (;;) {
            aster_app_mgr_files_lock();
            int error = aster_app_mgr_is_live(NULL) ? -EBUSY : aster_storage_format_and_reboot();
            aster_app_mgr_files_unlock();
            k_mutex_lock(&lock, K_FOREVER);
            aster_setup_core_fail(&snapshot.state, error ? error : -EIO);
            k_mutex_unlock(&lock);
            while (!consume(&retry_command))
                k_msleep(100);
            phase(ASTER_SETUP_FORMAT);
        }
    }
    bool force = forced_boot;
    forced_boot = false;
    if (!force && healthy()) {
        phase(ASTER_SETUP_DONE);
        return;
    }
    k_mutex_lock(&lock, K_FOREVER);
    snapshot.state.required = true;
    k_mutex_unlock(&lock);
    context *c = NULL;
    int rc = 0;
    bool welcome = true, marked = false;
    for (;;) {
        if (!c) {
            c = aster_psram_alloc(sizeof(*c));
            if (c)
                memset(c, 0, sizeof(*c));
            else {
                rc = -ENOMEM;
                goto failed;
            }
        }
        if (!aster_storage_mounted()) {
            phase(ASTER_SETUP_STORAGE);
            while (!consume(&retry_command))
                k_msleep(100);
            if ((rc = aster_storage_init()))
                goto failed;
            aster_app_mgr_scan();
            (void)aster_plat_wifi_storage_ready();
        }
        if (!marked) {
            const uint8_t pending_byte = 1;
            rc = aster_kv_set("aster.setup", "pending", &pending_byte, sizeof(pending_byte));
            if (rc)
                goto failed;
            marked = true;
        }
        if (welcome) {
            /* Interrupted first boot can continue with the station's saved profile;
             * a healthy installation never waits for network availability. */
            aster_setup_snapshot saved;
            aster_setup_get(&saved);
            /* The platform loads/queues its saved profile before this worker.
             * last_ssid() may return EBUSY during that boot join; that does not
             * mean the profile is missing. Wait for the asynchronous join. */
            if (!force && aster_setup_server_url(saved.server)) {
                welcome = false;
                strcpy(c->server, saved.server);
                phase(ASTER_SETUP_CONNECT);
                uint64_t until = k_uptime_get() + 40000;
                while (!connected() && k_uptime_get() < until)
                    k_msleep(100);
                rc = connected() ? download(c) : -ETIMEDOUT;
                if (!rc) {
                    phase(ASTER_SETUP_DONE);
                    aster_psram_free(c);
                    return;
                }
                goto failed;
            }
            phase(ASTER_SETUP_WELCOME);
            while (!consume(&begin_command))
                k_msleep(100);
            welcome = false;
        }
        rc = provision();
        if (rc)
            goto failed;
        for (;;) {
            aster_setup_credentials_t credentials;
            while (!take_credentials(&credentials))
                k_msleep(100);
            snprintf(c->server, sizeof(c->server), "%s", credentials.server);
            size_t n = strlen(c->server);
            while (n && c->server[n - 1] == '/')
                c->server[--n] = 0;
            rc = aster_kv_set("aster.setup", "server", c->server, strlen(c->server));
            if (!rc)
                rc = join(&credentials);
            memset(&credentials, 0, sizeof(credentials));
            if (!rc)
                rc = download(c);
            if (!rc) {
                phase(ASTER_SETUP_DONE);
                aster_psram_free(c);
                return;
            }
        failed:
            k_mutex_lock(&lock, K_FOREVER);
            aster_setup_core_fail(&snapshot.state, rc);
            k_mutex_unlock(&lock);
            LOG_ERR("setup failed: %d", rc);
            for (;;) {
                k_mutex_lock(&lock, K_FOREVER);
                bool pending_credentials = credentials_command;
                k_mutex_unlock(&lock);
                if (pending_credentials)
                    break;
                if (consume(&retry_command)) {
                    if (!aster_storage_mounted() || !c)
                        goto restart;
                    rc = provision();
                    if (rc)
                        goto failed;
                    break;
                }
                k_msleep(100);
            }
        }
    restart:
        if (!aster_storage_mounted()) {
            rc = aster_storage_init();
            if (rc)
                goto failed;
            aster_app_mgr_scan();
            (void)aster_plat_wifi_storage_ready();
        }
    }
}
bool aster_setup_poll(aster_setup_snapshot *out) {
    if (started && !stack && snapshot.state.phase == ASTER_SETUP_ERROR && consume(&retry_command))
        started = false;
    if (started && consume(&repair_command)) {
        if (stack && k_thread_join(&thread, K_NO_WAIT)) {
            k_mutex_lock(&lock, K_FOREVER);
            repair_command = true;
            *out = snapshot;
            k_mutex_unlock(&lock);
            return true;
        }
        if (stack) {
            aster_dram_free(stack);
            stack = NULL;
        }
        started = false;
        forced_boot = true;
        begin_command = true;
    }
    if (!started) {
        started = true;
        began = k_uptime_get();
        done_since = 0;
        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.state.phase = ASTER_SETUP_CHECK;
        uint8_t random[20];
        if (sys_csrand_get(random, sizeof(random))) {
            aster_setup_core_fail(&snapshot.state, -EIO);
            *out = snapshot;
            return true;
        }
        snprintf(snapshot.ssid, sizeof(snapshot.ssid), "Aster-Setup-%02X%02X", random[0],
                 random[1]);
        for (unsigned i = 0; i < 12; i++)
            snapshot.password[i] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"[random[i] % 31];
        for (unsigned i = 0; i < 16; i++)
            snprintf(snapshot.token + i * 2, 3, "%02x", random[i + 4]);
        size_t n = 0;
        if (aster_kv_get("aster.setup", "server", snapshot.server, sizeof(snapshot.server) - 1,
                         &n) ||
            n >= sizeof(snapshot.server))
            snapshot.server[0] = 0;
        stack = aster_dram_alloc(K_KERNEL_STACK_LEN(STACK));
        if (!stack) {
            snapshot.state.required = true;
            aster_setup_core_fail(&snapshot.state, -ENOMEM);
        } else {
            k_thread_create(&thread, stack, STACK, worker, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0,
                            K_NO_WAIT);
            k_thread_name_set(&thread, "aster_setup");
        }
    }
    k_mutex_lock(&lock, K_FOREVER);
    *out = snapshot;
    out->seconds = (k_uptime_get() - began) / 1000;
    k_mutex_unlock(&lock);
    if (out->state.phase == ASTER_SETUP_DONE) {
        /* Give the phone's status poll time to observe success before the
         * temporary AP disappears. Healthy/offline boots have no hotspot. */
        if (out->hotspot) {
            if (!done_since)
                done_since = k_uptime_get();
            if (k_uptime_get() - done_since < 4000)
                return true;
        }
        if (!aster_setup_web_stop())
            return true;
        if (stack && k_thread_join(&thread, K_NO_WAIT))
            return true;
        if (stack) {
            aster_dram_free(stack);
            stack = NULL;
        }
        if (out->hotspot) {
            aster_plat_wifi_hotspot_stop();
            k_mutex_lock(&lock, K_FOREVER);
            snapshot.hotspot = false;
            k_mutex_unlock(&lock);
        }
        return false;
    }
    return true;
}
void aster_setup_get(aster_setup_snapshot *out) {
    k_mutex_lock(&lock, K_FOREVER);
    *out = snapshot;
    out->seconds = (k_uptime_get() - began) / 1000;
    k_mutex_unlock(&lock);
}
