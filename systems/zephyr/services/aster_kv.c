/* SPDX-License-Identifier: MIT */
#include "aster_kv.h"
#include "aster_storage.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aster_kv, LOG_LEVEL_INF);

#define KV_KEY_MAX 48

static int kv_path(char *out, size_t n, const char *id, const char *key)
{
	if (strlen(key) > KV_KEY_MAX) {
		return -1;
	}
	/* sanitize: no path separators */
	for (const char *p = key; *p; p++) {
		if (*p == '/' || *p == ':' || *p == '\\') {
			return -1;
		}
	}
	int w = snprintf(out, n, "%s/%s.%s", ASTER_KV_DIR, id, key);
	return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

int aster_kv_set(const char *id, const char *key, const void *val, size_t len)
{
	char path[160];
	if (kv_path(path, sizeof(path), id, key)) {
		return -EINVAL;
	}
	struct fs_file_t f;
	fs_file_t_init(&f);
	int err = fs_open(&f, path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
	if (err) {
		return err;
	}
	err = aster_fs_write_safe(&f, val, len);
	fs_close(&f);
	return err < 0 ? err : 0;
}

int aster_kv_get(const char *id, const char *key, void *dst, size_t max, size_t *out_len)
{
	char path[160];
	if (kv_path(path, sizeof(path), id, key)) {
		return -ENOENT;
	}
	/* a missing key is normal (first read); fs_open would log it at <err> */
	if (fs_stat(path, &(struct fs_dirent){0}) != 0) {
		return -ENOENT;
	}
	struct fs_file_t f;
	fs_file_t_init(&f);
	int err = fs_open(&f, path, FS_O_READ);
	if (err) {
		return err;
	}
	err = aster_fs_read_safe(&f, dst, max);
	fs_close(&f);
	if (err < 0) {
		return err;
	}
	if (out_len) {
		*out_len = (size_t)err;
	}
	return 0;
}

int aster_kv_delete(const char *id, const char *key)
{
	char path[160];
	if (kv_path(path, sizeof(path), id, key)) {
		return -EINVAL;
	}
	int err = fs_unlink(path);
	return (err == -ENOENT) ? 0 : err;
}
