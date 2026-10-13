/* SPDX-License-Identifier: MIT */
#include "aster_storage.h"
#include "aster_versions.h"
#include "aster_app_mgr.h"
#include "aster_perms.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>
#include <esp_memory_utils.h>

LOG_MODULE_REGISTER(aster_storage, LOG_LEVEL_INF);

static bool mounted;

/* ---- DMA-safe file I/O ---------------------------------------------------
 * Some SDMMC controllers only accept DMA-capable (internal RAM) buffers, but
 * ASTER keeps wasm images and linear memory in external RAM. When the
 * destination or source is not DMA-capable we shuttle the data through an
 * internal-RAM bounce buffer; otherwise we pass the caller's buffer through. */
/* 4 KB: PSRAM targets (font tables, images) are not DMA-capable and go
 * through this bounce; at 1 KB a 32 KB cpfont metric chunk cost 32
 * stop-and-wait SD reads (~0.5 MB/s wall). 4 KB cuts the per-read overhead
 * 4x on the boot-critical font streaming path. Internal DRAM has room. */
#define ASTER_BOUNCE 4096
static uint8_t bounce[ASTER_BOUNCE] __aligned(32);
static K_MUTEX_DEFINE(bounce_lock);

static bool dma_capable(const void *p)
{
	return ((((uintptr_t)p) % 4) == 0) && esp_ptr_dma_capable(p);
}

int aster_fs_read_safe(struct fs_file_t *f, void *dst, size_t len)
{
	if (dma_capable(dst)) {
		return fs_read(f, dst, len);
	}
	uint8_t *out = dst;
	size_t done = 0;
	int err = 0;

	k_mutex_lock(&bounce_lock, K_FOREVER);
	while (done < len) {
		size_t chunk = len - done;
		if (chunk > ASTER_BOUNCE) {
			chunk = ASTER_BOUNCE;
		}
		int r = fs_read(f, bounce, chunk);
		if (r < 0) {
			err = r;
			break;
		}
		if (r == 0) {
			break; /* EOF */
		}
		memcpy(out + done, bounce, (size_t)r);
		done += (size_t)r;
		if ((size_t)r < chunk) {
			break; /* short read: EOF */
		}
	}
	k_mutex_unlock(&bounce_lock);
	return done ? (int)done : err;
}

int aster_fs_write_safe(struct fs_file_t *f, const void *src, size_t len)
{
	if (dma_capable(src)) {
		return fs_write(f, src, len);
	}
	const uint8_t *in = src;
	size_t done = 0;
	int err = 0;

	k_mutex_lock(&bounce_lock, K_FOREVER);
	while (done < len) {
		size_t chunk = len - done;
		if (chunk > ASTER_BOUNCE) {
			chunk = ASTER_BOUNCE;
		}
		memcpy(bounce, in + done, chunk);
		int r = fs_write(f, bounce, chunk);
		if (r < 0) {
			err = r;
			break;
		}
		if (r == 0) {
			err = -EIO;
			break;
		}
		done += (size_t)r;
		if ((size_t)r < chunk) {
			break;
		}
	}
	k_mutex_unlock(&bounce_lock);
	return done ? (int)done : err;
}
/* FatFs per-volume work object: fs_mount(FS_FATFS) requires mountp->fs_data
 * to point at it, otherwise f_mount() finds no registered filesystem object
 * and returns FR_NOT_ENABLED (mapped to -ENODEV). */
static FATFS fat_fs;

/* Zephyr's fs_mkdir logs at <err> level on EEXIST. ASTER creates the same hot
 * dirs (apps/, /data, ...) over and over, so probe with fs_stat first to keep
 * the console clean; only a real mkdir failure is returned. */
static int ensure_one(const char *dir)
{
	struct fs_dirent st;
	if (fs_stat(dir, &st) == 0) {
		return (st.type == FS_DIR_ENTRY_DIR) ? 0 : -ENOTDIR;
	}
	int err = fs_mkdir(dir);
	return (err == -EEXIST) ? 0 : err;
}

int aster_storage_mkdir_p(const char *path)
{
	char tmp[256];
	size_t n = strlen(path);
	if (n >= sizeof(tmp)) {
		return -EINVAL;
	}
	strcpy(tmp, path);
	if (n > 0 && tmp[n - 1] == '/') {
		tmp[n - 1] = 0;
		n--;
	}

	for (size_t i = 1; i < n; i++) {
		if (tmp[i] == '/') {
			tmp[i] = 0;
			/* "/SD:/..." — the segment ending in ':' is the volume
			 * root, fs_mkdir on it fails with EINVAL; skip it */
			if (tmp[i - 1] != ':') {
				int err = ensure_one(tmp);
				if (err) {
					return err;
				}
			}
			tmp[i] = '/';
		}
	}
	return ensure_one(tmp);
}

static int ensure_dirs(void)
{
	int err = aster_storage_mkdir_p(ASTER_APPS_DIR);
	if (!err) {
		err = aster_storage_mkdir_p(ASTER_INSTALL_DIR);
	}
	if (!err) {
		err = aster_storage_mkdir_p(ASTER_KV_DIR);
	}
	if (!err) {
		err = aster_storage_mkdir_p(ASTER_MUSIC_DIR);
	}
	if (!err) {
		err = aster_storage_mkdir_p(ASTER_FONTS_DIR);
	}
	return err;
}

bool aster_storage_is_music_path(const char *app_path)
{
	if (!app_path) {
		return false;
	}
	return strncmp(app_path, "/music", 6) == 0 &&
	       (app_path[6] == '\0' || app_path[6] == '/');
}

int aster_storage_init(void)
{
	int err;

	if (mounted) {
		return 0;
	}

	err = disk_access_ioctl("SD", DISK_IOCTL_CTRL_INIT, NULL);
	if (err) {
		LOG_WRN("SD init failed (%d) — no card?", err);
		return err;
	}

	static struct fs_mount_t mp = {
		.type = FS_FATFS,
		.mnt_point = ASTER_MOUNT_POINT,
		.fs_data = &fat_fs,
	};

	err = fs_mount(&mp);
	if (err) {
		LOG_ERR("SD mount failed (%d)", err);
		(void)disk_access_ioctl("SD", DISK_IOCTL_CTRL_DEINIT, NULL);
		return err;
	}
	mounted = true;

	err = ensure_dirs();
	if (err) {
		LOG_ERR("aster dirs: %d", err);
		return err;
	}

	uint32_t blocks, bsize;
	if (disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_COUNT, &blocks) == 0 &&
	    disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_SIZE, &bsize) == 0) {
		LOG_INF("SD mounted: %llu MB",
			((uint64_t)blocks * bsize) >> 20);
	} else {
		LOG_INF("SD mounted");
	}
	return 0;
}

bool aster_storage_mounted(void)
{
	return mounted;
}

void aster_storage_app_dir(char *buf, size_t n, const char *id)
{
	snprintf(buf, n, "%s/%s", ASTER_APPS_DIR, id);
}

int aster_storage_app_code_dir(char *buf, size_t n, const char *id)
{
	char base[128];
	aster_storage_app_dir(base, sizeof(base), id);
	struct aster_version version;
	if (aster_app_mgr_code_slot(id, &version.slot)) {
		return aster_versions_dir(buf, n, base, version.slot);
	}
	int err = aster_versions_find(base, &version);
	return err ? err : aster_versions_dir(buf, n, base, version.slot);
}

int aster_storage_sandbox(char *out, size_t n, const char *id, const char *app_path,
			uint32_t perm)
{
	const char *sub;

	if (!app_path || app_path[0] != '/') {
		return -EINVAL;
	}
	if (strstr(app_path, "..")) {
		return -EINVAL; /* no escape */
	}
	if (strcmp(app_path, "/sd") == 0 || strncmp(app_path, "/sd/", 4) == 0) {
		int w;

		if ((perm & ASTER_PERM_SD) == 0) {
			return -EPERM;
		}
		if (app_path[3] == '\0' || app_path[4] == '\0') {
			w = snprintf(out, n, "%s", ASTER_MOUNT_POINT);
		} else {
			w = snprintf(out, n, "%s%s", ASTER_MOUNT_POINT, app_path + 3);
		}
		if (w < 0 || (size_t)w >= n) {
			return -ENAMETOOLONG;
		}
		return 0;
	}
	if (aster_storage_is_music_path(app_path)) {
		int w = snprintf(out, n, "%s%s", ASTER_MUSIC_DIR, app_path + 6);
		if (w < 0 || (size_t)w >= n) {
			return -ENAMETOOLONG;
		}
		return 0;
	}
	if (strncmp(app_path, "/data", 5) == 0) {
		sub = "/data";
	} else if (strncmp(app_path, "/cache", 6) == 0) {
		sub = "/cache";
	} else if (strncmp(app_path, "/assets", 7) == 0) {
		sub = "/assets";
	} else {
		return -EINVAL;
	}

	char root[96];
	if (strcmp(sub, "/assets") == 0) {
		int err = aster_storage_app_code_dir(root, sizeof(root), id);
		if (err) { return err; }
	} else {
		aster_storage_app_dir(root, sizeof(root), id);
	}
	int w = snprintf(out, n, "%s%s%s", root, sub, app_path + strlen(sub));
	if (w < 0 || (size_t)w >= n) {
		return -ENAMETOOLONG;
	}
	/* create the leaf dir's parents on demand */
	return 0;
}
