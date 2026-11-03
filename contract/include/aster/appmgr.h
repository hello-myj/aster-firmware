/* SPDX-License-Identifier: MIT
 * ASTER ABI 1.2 - app manager (launcher / system-UI surface).
 *
 * Gated by the manifest permission "appmgr" (hard-enforced since 1.2):
 * callers whose manifest lacks it receive ASTER_EPERM.
 */
#ifndef ASTER_APPMGR_H
#define ASTER_APPMGR_H

#include "types.h"

/* Registry record: stable metadata + live state of one installed app. */
#define ASTER_APPST_STOPPED 0u
#define ASTER_APPST_RUNNING 1u
#define ASTER_APPST_PAUSED  2u
#define ASTER_APPST_CRASHED 3u

typedef struct {
	uint32_t size;    /* sizeof(aster_app_info_t) */
	uint32_t version; /* 1: through flags; 2: + category / has_icon */
	char     id[64];         /* reverse-domain app id (= install dir) */
	char     name[40];       /* display name */
	char     app_version[24];/* the app's own semantic version */
	uint32_t state;          /* ASTER_APPST_x */
	uint32_t has_aot;        /* package carries a matching AOT image */
	uint32_t flags;          /* reserved, 0 */
	char     category[16];   /* "game" / "tool" / "system" / empty */
	uint32_t has_icon;       /* 1 if /apps/<id>/icon.bin exists */
} aster_app_info_t;

/* Number of installed apps (0..16). */
ASTER_I("aster_appmgr_count") int32_t aster_appmgr_count(void);

/* Fill info for the index-th app (insertion order). ASTER_EINVAL on bad index. */
ASTER_I("aster_appmgr_list") int32_t aster_appmgr_list(uint32_t index,
						 aster_app_info_t *info,
						 uint32_t info_size);

/* Launch another app: the caller is cleanly torn down first (stop/destroy,
 * resources reclaimed), then `id` starts -- the "open app" handoff of the
 * single-app runtime. ASTER_EINVAL for self, ASTER_ENOENT unknown id,
 * ASTER_EBUSY if something else started meanwhile. */
ASTER_I("aster_appmgr_start") int32_t aster_appmgr_start(const char *id, uint32_t id_len);

/* Uninstall a package by id. Refuses the caller's own id and any running or
 * paused app. */
ASTER_I("aster_appmgr_uninstall") int32_t aster_appmgr_uninstall(const char *id,
							   uint32_t id_len);

/* ABI 4.3. Requires appmgr + fs + kv. Both paths are in the caller's /data.
 * Close all download files first. Accepting the request cleanly exits the
 * caller; installation runs after VM teardown and then reopens the caller.
 * Resources are a directory tree copied into the inactive code slot's assets
 * before committing it. KV "store.install.result" receives the int32 result.
 * Self-update is rejected. A failed install leaves the prior slot active. */
ASTER_I("aster_appmgr_install") int32_t aster_appmgr_install(const char *package, uint32_t package_len,
    const char *resources, uint32_t resources_len);

#endif /* ASTER_APPMGR_H */
