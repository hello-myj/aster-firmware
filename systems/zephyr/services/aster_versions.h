/* SPDX-License-Identifier: MIT
 * Private on-disk APP generations. User data stays in the stable APP root.
 * No APP ABI or ASTP format change. Callers serialize mutations with files_lock.
 */
#ifndef ASTER_VERSIONS_H
#define ASTER_VERSIONS_H
#include <stdint.h>
#include <stddef.h>
#define ASTER_VERSION_RECORD_SIZE 24
struct aster_version {
    int slot; /* -1: legacy files in the APP root; otherwise 0 or 1 */
    uint32_t sequence;
};
int aster_versions_find(const char *base, struct aster_version *version);
int aster_versions_prepare(const char *base, struct aster_version *version);
int aster_versions_dir(char *out, size_t cap, const char *base, int slot);
int aster_versions_manifest_crc(const char *dir, uint32_t *crc);
void aster_versions_record(uint8_t out[ASTER_VERSION_RECORD_SIZE], unsigned slot,
                           uint32_t sequence, uint32_t manifest_crc);
#endif
