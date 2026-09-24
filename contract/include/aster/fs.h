/* SPDX-License-Identifier: MIT */
#ifndef ASTER_FS_H
#define ASTER_FS_H

#include "types.h"

/* Open flags (subset of POSIX) */
#define ASTER_FS_O_RDONLY 0x01u
#define ASTER_FS_O_WRONLY 0x02u
#define ASTER_FS_O_RDWR   0x03u
#define ASTER_FS_O_CREAT  0x10u
#define ASTER_FS_O_TRUNC  0x20u
#define ASTER_FS_O_APPEND 0x40u

/* Seek whence */
#define ASTER_FS_SEEK_SET 0
#define ASTER_FS_SEEK_CUR 1
#define ASTER_FS_SEEK_END 2

typedef struct {
	uint32_t size;    /* sizeof(aster_stat_t) */
	uint32_t version; /* 1 */
	uint64_t filesize;
	uint32_t mode;    /* bit0 = dir */
	uint32_t reserved;
} aster_stat_t;

#define ASTER_STAT_DIR 1u

/* Paths are app-absolute inside the sandbox:
 *   /data /cache /assets  → /SD:/aster/apps/<id>/…
 *   /music                → /SD:/music  (shared library, read-only)
 *   /sd                   → /SD:        (requires permissions=["sd"])
 * The Runtime maps them (design doc sec.40). */
ASTER_I("aster_fs_open") int32_t aster_fs_open(const char *path, uint32_t path_len, uint32_t flags,
			 aster_handle_t *out);
ASTER_I("aster_fs_read") int32_t aster_fs_read(aster_handle_t file, void *dst, uint32_t len);
ASTER_I("aster_fs_write") int32_t aster_fs_write(aster_handle_t file, const void *src, uint32_t len);
ASTER_I("aster_fs_seek") int32_t aster_fs_seek(aster_handle_t file, int32_t ofs, uint32_t whence);
ASTER_I("aster_fs_close") int32_t aster_fs_close(aster_handle_t file);
ASTER_I("aster_fs_mkdir") int32_t aster_fs_mkdir(const char *path, uint32_t path_len);
ASTER_I("aster_fs_unlink") int32_t aster_fs_unlink(const char *path, uint32_t path_len);
ASTER_I("aster_fs_stat") int32_t aster_fs_stat(const char *path, uint32_t path_len, aster_stat_t *st,
				uint32_t st_size);
/* Opaque dir entry cursor: call repeatedly; returns entry name length or 0 = end, <0 err. */
ASTER_I("aster_fs_dir_next") int32_t aster_fs_dir_next(aster_handle_t dir, char *name, uint32_t name_max,
					uint32_t *is_dir);
ASTER_I("aster_fs_opendir") int32_t aster_fs_opendir(const char *path, uint32_t path_len, aster_handle_t *out);
ASTER_I("aster_fs_closedir") int32_t aster_fs_closedir(aster_handle_t dir);

#endif /* ASTER_FS_H */
