/* SPDX-License-Identifier: MIT
 * Permission model (ABI 1.2).
 *
 * A manifest's `permissions` array is parsed into a bitmask at install and
 * scan time; the runtime hard-gates each API family on it (ASTER_EPERM when
 * absent). 1.2 turned permissions from "informational" into enforced; V1
 * apps that used an API without declaring it must add the declaration
 * (all in-tree samples do).
 */
#ifndef ASTER_PERMS_H
#define ASTER_PERMS_H

#include <stdint.h>
#include <string.h>

#define ASTER_PERM_FS      (1u << 0)
#define ASTER_PERM_KV      (1u << 1)
#define ASTER_PERM_APPMGR  (1u << 2)
#define ASTER_PERM_SD      (1u << 3) /* /sd → /SD: (no ABI bump) */

/* Map a permission token to its bit (0 if unknown). */
static inline uint32_t aster_perm_bit(const char *name)
{
	if (!strcmp(name, "fs")) {
		return ASTER_PERM_FS;
	}
	if (!strcmp(name, "kv")) {
		return ASTER_PERM_KV;
	}
	if (!strcmp(name, "appmgr")) {
		return ASTER_PERM_APPMGR;
	}
	if (!strcmp(name, "sd")) {
		return ASTER_PERM_SD;
	}
	return 0;
}

/* Pack a permissions[] array into a mask. */
static inline uint32_t aster_perm_mask(const char arr[][24], uint32_t n)
{
	uint32_t m = 0;
	for (uint32_t i = 0; i < n; i++) {
		m |= aster_perm_bit(arr[i]);
	}
	return m;
}

#endif /* ASTER_PERMS_H */
