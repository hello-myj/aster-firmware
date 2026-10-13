/* SPDX-License-Identifier: MIT */
#include "aster_versions.h"
#include "aster_pkg.h"
#include "aster_storage.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Two independent commit records are bound to an immutable, complete slot by
 * manifest CRC. A torn inactive record cannot overwrite the current record.
 * This detects incomplete metadata; physical FAT/SD power-loss semantics still
 * require board tests. CRC is integrity checking, not signature verification. */
static uint32_t version_crc_update(uint32_t crc, const uint8_t *data, size_t len)
{
    while (len--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}
static uint32_t version_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void version_put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) { p[i] = (uint8_t)(v >> (i * 8)); }
}
int aster_versions_dir(char *out, size_t cap, const char *base, int slot)
{
    if (slot < -1 || slot > 1) { return -EINVAL; }
    int n = slot < 0 ? snprintf(out, cap, "%s", base) :
                      snprintf(out, cap, "%s/.v%d", base, slot);
    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}
int aster_versions_manifest_crc(const char *dir, uint32_t *out)
{
    char path[192];
    struct fs_dirent st;
    if (snprintf(path, sizeof(path), "%s/%s", dir, ASTER_INSTALL_MARKER) >= (int)sizeof(path)) {
        return -ENAMETOOLONG;
    }
    int err = fs_stat(path, &st);
    if (err != -ENOENT) { return err ? err : -ENOENT; }
    if (snprintf(path, sizeof(path), "%s/manifest.json", dir) >= (int)sizeof(path)) {
        return -ENAMETOOLONG;
    }
    err = fs_stat(path, &st);
    if (err) { return err; }
    if (st.type != FS_DIR_ENTRY_FILE || !st.size || st.size >= ASTER_MANIFEST_MAX) {
        return -EBADMSG;
    }
    struct fs_file_t f;
    fs_file_t_init(&f);
    err = fs_open(&f, path, FS_O_READ);
    if (err) { return err; }
    uint8_t chunk[256];
    uint32_t crc = ~0u;
    size_t left = st.size;
    while (left) {
        size_t n = left < sizeof(chunk) ? left : sizeof(chunk);
        int read = aster_fs_read_safe(&f, chunk, n);
        if (read < 0 || (size_t)read != n) { err = read < 0 ? read : -EIO; break; }
        crc = version_crc_update(crc, chunk, n);
        left -= n;
    }
    int close = fs_close(&f);
    if (err || close) { return err ? err : close; }
    *out = ~crc;
    return 0;
}
void aster_versions_record(uint8_t out[ASTER_VERSION_RECORD_SIZE], unsigned slot,
                           uint32_t sequence, uint32_t manifest_crc)
{
    memcpy(out, "ASTV", 4);
    version_put32(out + 4, 1);
    version_put32(out + 8, sequence);
    version_put32(out + 12, slot);
    version_put32(out + 16, manifest_crc);
    version_put32(out + 20, ~version_crc_update(~0u, out, 20));
}
static int version_candidate(const char *base, unsigned slot, uint32_t *sequence)
{
    char path[192], dir[128];
    uint8_t record[ASTER_VERSION_RECORD_SIZE];
    struct fs_dirent st;
    if (snprintf(path, sizeof(path), "%s/.active%u", base, slot) >= (int)sizeof(path)) {
        return -ENAMETOOLONG;
    }
    int err = fs_stat(path, &st);
    if (err) { return err; }
    if (st.type != FS_DIR_ENTRY_FILE || st.size != sizeof(record)) { return -EBADMSG; }
    struct fs_file_t f;
    fs_file_t_init(&f);
    err = fs_open(&f, path, FS_O_READ);
    if (err) { return err; }
    int read = aster_fs_read_safe(&f, record, sizeof(record));
    int close = fs_close(&f);
    if (read < 0 || (size_t)read != sizeof(record) || close) { return -EIO; }
    if (memcmp(record, "ASTV", 4) || version_u32(record + 4) != 1 ||
        version_u32(record + 12) != slot ||
        version_u32(record + 20) != ~version_crc_update(~0u, record, 20)) {
        return -EBADMSG;
    }
    err = aster_versions_dir(dir, sizeof(dir), base, (int)slot);
    if (err) { return err; }
    uint32_t crc;
    err = aster_versions_manifest_crc(dir, &crc);
    if (err) { return err; }
    if (crc != version_u32(record + 16)) { return -EBADMSG; }
    /* Byte-for-byte payload verification precedes commit. Recovery also
     * refuses slots missing required program files or a valid AOT header. */
    const char *files[] = {"package.ast", "app.wasm", "app.aot"};
    for (unsigned i = 0; i < 3; i++) {
        if (snprintf(path, sizeof(path), "%s/%s", dir, files[i]) >= (int)sizeof(path)) {
            return -ENAMETOOLONG;
        }
        err = fs_stat(path, &st);
        if (err) { return err; }
        if (st.type != FS_DIR_ENTRY_FILE || st.size < 8) { return -EBADMSG; }
    }
    fs_file_t_init(&f);
    err = fs_open(&f, path, FS_O_READ);
    if (err) { return err; }
    uint8_t magic[4];
    read = aster_fs_read_safe(&f, magic, sizeof(magic));
    close = fs_close(&f);
    if (read != 4 || close) { return -EIO; }
    if (memcmp(magic, "\0aot", 4)) { return -EBADMSG; }
    *sequence = version_u32(record + 8);
    return 0;
}
static int version_find(const char *base, struct aster_version *version, bool strict)
{
    bool found = false, uncertain = false;
    for (unsigned slot = 0; slot < 2; slot++) {
        uint32_t sequence;
        int err = version_candidate(base, slot, &sequence);
        if (err) {
            if (err != -ENOENT && err != -EBADMSG) { uncertain = true; }
            continue;
        }
        /* Wrap-safe ordering for consecutive u32 sequence numbers. */
        if (!found || (sequence != version->sequence &&
                       (uint32_t)(sequence - version->sequence) < 0x80000000u)) {
            version->slot = (int)slot;
            version->sequence = sequence;
            found = true;
        }
    }
    if (strict && uncertain) { return -EIO; }
    if (found) { return 0; }
    if (uncertain) { return -EIO; } /* Never erase a slot after uncertain I/O. */
    uint32_t crc;
    int err = aster_versions_manifest_crc(base, &crc);
    if (err) { return err; }
    version->slot = -1;
    version->sequence = 0;
    return 0;
}

int aster_versions_find(const char *base, struct aster_version *version)
{
    return version_find(base, version, false);
}
int aster_versions_prepare(const char *base, struct aster_version *version)
{
    /* A transient error reading the current slot must not cause the installer
     * to mistake it for inactive and erase it. Recovery may use another slot;
     * mutation waits until BOTH records have an unambiguous status. */
    return version_find(base, version, true);
}
