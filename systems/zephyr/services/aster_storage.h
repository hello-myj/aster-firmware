/* SPDX-License-Identifier: MIT
 * Storage: SD mount + ASTER directory conventions (design doc sec.74).
 *
 * /SD:/aster/apps/<id>/{data,cache,.v0,.v1,.active0,.active1}
 * Legacy flat code/assets are still readable. See docs/package-installation.md.
 * /SD:/aster/install/   (drop directory for .ast files)
 * /SD:/aster/system/kv  (per-app kv files: <id>.<key>)
 */
#ifndef ASTER_STORAGE_H
#define ASTER_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <zephyr/fs/fs.h>

#define ASTER_SD_ROOT     "/SD:"
#define ASTER_MOUNT_POINT "/SD:"
#define ASTER_DIR         "/SD:/aster"
#define ASTER_APPS_DIR    "/SD:/aster/apps"
#define ASTER_INSTALL_DIR "/SD:/aster/install"
#define ASTER_KV_DIR      "/SD:/aster/system/kv"
#define ASTER_MUSIC_DIR   "/SD:/music"
#define ASTER_FONTS_DIR   "/SD:/aster/fonts"

int  aster_storage_init(void);      /* mount FAT, create dirs */
bool aster_storage_mounted(void);
/* /SD:/aster/apps/<id> */
void aster_storage_app_dir(char *buf, size_t n, const char *id);
/* Selected immutable code/assets directory (legacy root or .v0/.v1). */
int aster_storage_app_code_dir(char *buf, size_t n, const char *id);
/* sandbox path mapping:
 *   /data /cache → stable /SD:/aster/apps/<id>/…
 *   /assets → selected immutable code slot (or legacy flat assets)
 *   /music               → /SD:/music (shared, read-only for apps)
 *   /sd                  → /SD:  (requires ASTER_PERM_SD; still no "..")
 * `perm` is the app's ASTER_PERM_* mask. */
int  aster_storage_sandbox(char *out, size_t n, const char *id, const char *app_path,
			 uint32_t perm);
/* True for the shared "/music" tree (apps may list/play, not write). */
bool aster_storage_is_music_path(const char *app_path);
/* ensure a directory exists (mkdir -p) */
int  aster_storage_mkdir_p(const char *path);

/* Is this a name a FAT directory entry could legitimately carry?
 * 0x00-0x1F are illegal in both short and long FAT names, so an entry whose
 * name contains one is not a file. The bench card's /SD: root hands back a
 * whole sector of such entries ahead of the real ones -- byte0 == 0x01 (an
 * LFN sequence byte) on all 16, empty as a string, with a garbage size and
 * sometimes the DIR bit set. Every directory walker has to drop them, or the
 * shell lists 16 phantom rows and an app browser offers dead entries.
 *
 * A lone "?" is the same entries arriving by another road. f_readdir (ff.c,
 * "inaccessible object name") substitutes exactly that when an entry's long
 * *and* short name both fail to decode, which is what those bytes do: their
 * short name leads with 0x01, a character no code page maps. Since names are
 * UTF-8 end to end the long-name path no longer throws them away first, so
 * the filter has to know the placeholder by name. A real file called "?" is
 * not reachable through this API anyway -- FatFs never decodes what it
 * reports as inaccessible, and f_open() on "?" opens nothing.
 *
 * Callers still treat name[0] == 0 as the end-of-directory sentinel. */
static inline bool aster_fs_name_valid(const char *n)
{
	if (n[0] == '?' && n[1] == 0) {
		return false;
	}
	for (; *n; n++) {
		if ((unsigned char)*n < 0x20) {
			return false;
		}
	}

	return true;
}

/* DMA-safe file I/O: some SDMMC controllers reject external/PSRAM buffers,
 * but wasm linear memory / loaded images live there. These helpers copy
 * through an internal-RAM bounce buffer. Use for ANY fs_read/fs_write whose
 * target may be in non-DMA-capable memory. Returns bytes transferred (>=0)
 * or negative errno. */
int aster_fs_read_safe(struct fs_file_t *f, void *dst, size_t len);
int aster_fs_write_safe(struct fs_file_t *f, const void *src, size_t len);

#endif
