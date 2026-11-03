/* SPDX-License-Identifier: MIT */
#include "aster_pkg.h"
#include "aster_versions.h"
#include "aster_app_mgr.h"
#include "aster_sig.h"
#include "aster_storage.h"
#include "aster_sysinfo.h"
#include "aster_mem.h"
#include "aster_bytes.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#ifdef CONFIG_ASTER_PKG_FAULT_INJECTION
#include <zephyr/sys/reboot.h>
#if defined(CONFIG_SOC_SERIES_ESP32S3)
#include <esp_rom_sys.h>
#endif
#endif
#include <zephyr/logging/log.h>
#include <aster/aster.h>
#include <aster/icon.h>

LOG_MODULE_REGISTER(aster_pkg, LOG_LEVEL_INF);

struct pkg_header {
	uint8_t  magic[4];
	uint16_t fmt_ver;
	uint16_t hdr_size;
	uint32_t flags;
	uint32_t manifest_off, manifest_len;
	uint32_t wasm_off, wasm_len;
	uint32_t aot_off, aot_len;
	uint32_t aot_target;
	uint32_t icon_off, icon_len;
	uint16_t icon_w, icon_h;
	uint32_t ui_off, ui_len;
};

static void package_diagnostic(bool error, const char *message)
{
    if (error) { LOG_ERR("%s", message); }
    else { LOG_WRN("%s", message); }
}
int aster_pkg_parse(const uint8_t *img, size_t size, aster_manifest_t *m,
                  const uint8_t **wasm, size_t *wasm_len,
                  const uint8_t **aot, size_t *aot_len, uint32_t *aot_target)
{
    const aster_pkg_policy_t policy = {
        .target_id = CONFIG_ASTER_TARGET_ID, .abi_version = ASTER_ABI_VERSION,
        .alloc = k_malloc, .release = k_free,
        .has_feature = aster_sysinfo_feature_by_name,
        .diagnostic = package_diagnostic,
    };
    return aster_pkg_decode(&policy,img,size,m,wasm,wasm_len,aot,aot_len,aot_target);
}

/* Load a whole file into memory */
static int load_file(const char *path, uint8_t **buf, size_t *len)
{
	struct fs_dirent st;
	int err = fs_stat(path, &st);
	if (err) {
		return err;
	}
	/* packages go to PSRAM: an LVGL-class app .ast (400 KB+) never fits the
	 * small internal k_malloc heap (same backing as the start-path loader) */
	uint8_t *b = aster_psram_alloc(st.size + 1);
	if (!b) {
		return -ENOMEM;
	}
	struct fs_file_t f;
	fs_file_t_init(&f);
	err = fs_open(&f, path, FS_O_READ);
	if (err) {
		aster_psram_free(b);
		return err;
	}
	err = aster_fs_read_safe(&f, b, (size_t)st.size);
	fs_close(&f);
	if (err < 0) {
		aster_psram_free(b);
		return err;
	}
	*buf = b;
	*len = (size_t)err;
	return 0;
}

/* append "key":[..] to a JSON object being streamed; returns chars written,
 * negative if it does not fit in `n`.
 *
 * Writes straight into `out` rather than through a fixed scratch buffer. It
 * used to render into `char tmp[96]`, which made 96 bytes a hidden cap on an
 * app's declared `requires`/`optional`/`permissions` -- and since the caller
 * chains these calls through `if (r >= 0)`, one overflow also skipped every
 * array AFTER it, so a package with enough `requires` would lose its
 * `permissions` and run with perm_mask 0. Nothing was logged. snprintf's
 * truncation is what keeps `out` in bounds here; a negative return is the
 * caller's signal to reject the whole package. */
/* Append a JSON string field without letting quotes/backslashes in app
 * metadata corrupt the normalized manifest read by the registry. */
static int json_string_field(char *out, size_t n, const char *key, const char *value)
{
	int w = snprintf(out, n, ",\"%s\":\"", key);
	if (w < 0 || (size_t)w >= n) return -1;
	size_t used = (size_t)w;
	for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
		if (*p < 0x20) return -1; /* descriptions are single-line text */
		if (*p == '"' || *p == '\\') {
			if (used + 1 >= n) return -1;
			out[used++] = '\\';
		}
		if (used + 1 >= n) return -1;
		out[used++] = (char)*p;
	}
	if (used + 2 > n) return -1;
	out[used++] = '"';
	out[used] = 0;
	return (int)used;
}

static int json_arr(char *out, size_t n, const char *key, const char arr[][24], uint32_t cnt)
{
	size_t used;
	int w;

	if (cnt == 0) {
		return 0;
	}

	w = snprintf(out, n, ",\"%s\":[", key);
	if (w < 0 || (size_t)w >= n) {
		return -1;
	}
	used = (size_t)w;

	for (uint32_t i = 0; i < cnt; i++) {
		w = snprintf(out + used, n - used, "%s\"%s\"", i ? "," : "", arr[i]);
		if (w < 0 || (size_t)w >= n - used) {
			return -1;
		}
		used += (size_t)w;
	}

	w = snprintf(out + used, n - used, "]");
	if (w < 0 || (size_t)w >= n - used) {
		return -1;
	}
	used += (size_t)w;

	return (int)used;
}

/* All installation files use DMA-safe writes and the same durable error gate.
 * A failed close is an installation failure even after a successful write. */
#ifdef CONFIG_ASTER_PKG_FAULT_INJECTION
static struct {
	char id[64], stage[20], operation[20], active[64];
	bool armed; unsigned action;
} pkg_fault;
int aster_pkg_fault_arm(const char *id, const char *stage, const char *operation, unsigned action)
{
	if (strncmp(id, "test.p0.", 8) || strlen(id) >= sizeof(pkg_fault.id) ||
	    strlen(stage) >= sizeof(pkg_fault.stage) || strlen(operation) >= sizeof(pkg_fault.operation)) {
		return ASTER_EINVAL;
	}
	aster_app_mgr_files_lock();
	snprintf(pkg_fault.id, sizeof(pkg_fault.id), "%s", id);
	snprintf(pkg_fault.stage, sizeof(pkg_fault.stage), "%s", stage);
	snprintf(pkg_fault.operation, sizeof(pkg_fault.operation), "%s", operation);
	pkg_fault.action = action;
	pkg_fault.armed = true;
	aster_app_mgr_files_unlock();
	return ASTER_OK;
}
void aster_pkg_fault_clear(void) { pkg_fault.armed = false; }
static bool install_fault(const char *stage, const char *operation)
{
	if (!pkg_fault.armed || strcmp(pkg_fault.active, pkg_fault.id) ||
	    strcmp(stage, pkg_fault.stage) || strcmp(operation, pkg_fault.operation)) { return false; }
	pkg_fault.armed = false;
	LOG_ERR("install: diagnostic fault stage=%s operation=%s reset=%d", stage, operation, pkg_fault.action);
	if (pkg_fault.action == 2) {
		LOG_ERR("POWER_CUT_READY id=%s stage=%s operation=%s", pkg_fault.active, stage, operation);
		for (;;) { k_msleep(1000); }
	}
	if (pkg_fault.action == 1) {
#if defined(CONFIG_SOC_SERIES_ESP32S3)
		/* Reset the full SoC for transaction interruption tests; esp_restart
		 * otherwise restarts CPUs while retaining some peripheral state. */
		esp_rom_software_reset_system();
#else
		sys_reboot(SYS_REBOOT_COLD);
#endif
	}
	return true;
}
#else
static bool install_fault(const char *stage, const char *operation)
{
	(void)stage; (void)operation; return false;
}
#endif

static int install_verify(const char *path, const void *data, size_t len,
                          const char *stage)
{
	struct fs_dirent st;
	int err = fs_stat(path, &st);
	if (err || st.type != FS_DIR_ENTRY_FILE || st.size != len) {
		LOG_ERR("install: stage=%s operation=verify-size path=%s result=%d", stage, path, err);
		return ASTER_EIO;
	}
	struct fs_file_t f;
	fs_file_t_init(&f);
	err = fs_open(&f, path, FS_O_READ);
	if (err) {
		LOG_ERR("install: stage=%s operation=verify-open path=%s result=%d", stage, path, err);
		return ASTER_EIO;
	}
	uint8_t chunk[512];
	const uint8_t *expected = data;
	size_t done = 0;
	while (done < len) {
		size_t n = len - done < sizeof(chunk) ? len - done : sizeof(chunk);
		int read = aster_fs_read_safe(&f, chunk, n);
		if (read < 0 || (size_t)read != n) {
			LOG_ERR("install: stage=%s operation=verify-read path=%s result=%d", stage, path, read);
			err = ASTER_EIO;
			break;
		}
		if (install_fault(stage, "verify")) { chunk[0] ^= 1u; }
		if (memcmp(chunk, expected + done, n)) {
			LOG_ERR("install: stage=%s operation=verify-content path=%s", stage, path);
			err = ASTER_ECORRUPT;
			break;
		}
		done += n;
	}
	int close = fs_close(&f);
	if (close) {
		LOG_ERR("install: stage=%s operation=verify-close path=%s result=%d", stage, path, close);
		err = ASTER_EIO;
	}
	return err;
}

static int install_write(const char *path, const void *data, size_t len,
                         const char *stage)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	int err = install_fault(stage, "open") ? -EIO : fs_open(&f, path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
	if (err) {
		LOG_ERR("install: stage=%s operation=open path=%s result=%d", stage, path, err);
		return ASTER_EIO;
	}
	size_t requested = install_fault(stage, "write") && len ? len - 1u : len;
	int written = aster_fs_write_safe(&f, data, requested);
	if (written < 0 || (size_t)written != len) {
		LOG_ERR("install: stage=%s operation=write path=%s result=%d expected=%u",
			stage, path, written, (unsigned)len);
		err = ASTER_EIO;
	} else {
		int sync = fs_sync(&f);
		if (install_fault(stage, "sync")) { sync = -EIO; }
		if (sync) {
			LOG_ERR("install: stage=%s operation=sync path=%s result=%d", stage, path, sync);
			err = ASTER_EIO;
		}
	}
	int close = fs_close(&f);
	if (install_fault(stage, "close")) { close = -EIO; }
	if (close) {
		LOG_ERR("install: stage=%s operation=close path=%s result=%d", stage, path, close);
		err = ASTER_EIO;
	}
	return err ? err : install_verify(path, data, len, stage);
}

static int rmrf(const char *path)
{
	struct fs_dir_t dir;
	struct fs_dirent st;
	int err = fs_stat(path, &st);
	if (err) {
		return err == -ENOENT ? 0 : err;
	}
	if (st.type != FS_DIR_ENTRY_DIR) {
		return fs_unlink(path);
	}
	fs_dir_t_init(&dir);
	err = fs_opendir(&dir, path);
	if (err) {
		return err;
	}
	while ((err = fs_readdir(&dir, &st)) == 0 && st.name[0]) {
		char child[256];
		if (snprintf(child, sizeof(child), "%s/%s", path, st.name) >= (int)sizeof(child)) {
			err = -ENAMETOOLONG;
			break;
		}
		err = st.type == FS_DIR_ENTRY_DIR ? rmrf(child) : fs_unlink(child);
		if (err) {
			break;
		}
	}
	int close = fs_closedir(&dir);
	if (err || close) {
		return err ? err : close;
	}
	return fs_unlink(path);
}

static bool asset_relpath_ok(const char *p)
{
	size_t n;

	if (!p || !p[0] || p[0] == '/' || p[0] == '\\') {
		return false;
	}
	n = strlen(p);
	if (n == 0 || n >= 96) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)p[i];

		if (c < 0x20 || c == '\\') {
			return false;
		}
		if (c == '.' && (i == 0 || p[i - 1] == '/')) {
			if (p[i + 1] == '.' && (p[i + 2] == 0 || p[i + 2] == '/')) {
				return false;
			}
			if (p[i + 1] == 0 || p[i + 1] == '/') {
				return false;
			}
		}
	}
	return true;
}

static int extract_mjr1(const uint8_t *blob, size_t len, const char *appdir)
{
	char root[192];
	uint16_t n;
	uint32_t table;

	if (len < 16 || memcmp(blob, "ASTR", 4) != 0 || aster_le16(blob + 4) != 1) {
		return ASTER_ECORRUPT;
	}
	n = aster_le16(blob + 6);
	if (n > 64) {
		return ASTER_ECORRUPT;
	}
	table = 16;
	if (table + (uint32_t)n * 12u > len) {
		return ASTER_ECORRUPT;
	}
	if (snprintf(root, sizeof(root), "%s/assets", appdir) >= (int)sizeof(root)) {
		return ASTER_ENOMEM;
	}
	if (aster_storage_mkdir_p(root)) {
		return ASTER_EIO;
	}
	for (uint16_t i = 0; i < n; i++) {
		const uint8_t *ent = blob + table + (uint32_t)i * 12u;
		uint32_t po = aster_le32(ent);
		uint32_t fo = aster_le32(ent + 4);
		uint32_t fl = aster_le32(ent + 8);
		const char *rel;
		char dest[256];
		char parent[256];
		int err;
		size_t max_rel;

		if (po >= len || fo > len || fl > len || fo + fl > len) {
			return ASTER_ECORRUPT;
		}
		rel = (const char *)(blob + po);
		max_rel = len - po;
		if (max_rel == 0 || !memchr(rel, 0, max_rel) || !asset_relpath_ok(rel)) {
			LOG_ERR("install: stage=assets operation=validate bad asset path");
			return ASTER_ECORRUPT;
		}
		if (snprintf(dest, sizeof(dest), "%s/%s", root, rel) >= (int)sizeof(dest)) {
			return ASTER_ENOMEM;
		}
		memcpy(parent, dest, sizeof(parent));
		parent[sizeof(parent) - 1] = 0;
		{
			char *slash = strrchr(parent, '/');

			if (slash) {
				*slash = 0;
				if (aster_storage_mkdir_p(parent)) {
					return ASTER_EIO;
				}
			}
		}
		err = install_write(dest, blob + fo, fl, "assets");
		if (err) {
			return err;
		}
	}
	LOG_INF("install: assets %u file(s) -> %s", n, root);
	return ASTER_OK;
}

static int copy_bundle_file(const char *src,const char *dst,uint32_t size)
{
    struct fs_dirent st;
    if(fs_stat(dst,&st)!=-ENOENT)return ASTER_EEXIST;
    struct fs_file_t input,output;fs_file_t_init(&input);fs_file_t_init(&output);
    if(fs_open(&input,src,FS_O_READ))return ASTER_EIO;
    if(fs_open(&output,dst,FS_O_CREATE|FS_O_WRITE|FS_O_TRUNC)){fs_close(&input);return ASTER_EIO;}
    uint8_t bytes[2048];int result=0;
    while(size) {
        unsigned n=size>sizeof(bytes)?sizeof(bytes):size;
        int got=fs_read(&input,bytes,n);
        if(got<=0 || (unsigned)got>n){result=ASTER_EIO;break;}
        unsigned done=0;
        while(done<(unsigned)got) {
            int written=fs_write(&output,bytes+done,(unsigned)got-done);
            if(written<=0 || (unsigned)written>(unsigned)got-done){result=ASTER_EIO;break;}
            done+=(unsigned)written;
        }
        if(result)break;
        size-=(unsigned)got;
    }
    if(!result && fs_read(&input,bytes,1)!=0)result=ASTER_EIO;
    if(!result && fs_sync(&output))result=ASTER_EIO;
    if(fs_close(&input))result=ASTER_EIO;
    if(fs_close(&output))result=ASTER_EIO;
    return result;
}
static int copy_bundle_tree(const char *src,const char *dst,unsigned depth,unsigned *count,uint64_t *bytes)
{
    if(depth>16)return ASTER_EINVAL;
    if(aster_storage_mkdir_p(dst))return ASTER_EIO;
    struct fs_dir_t dir;fs_dir_t_init(&dir);
    if(fs_opendir(&dir,src))return ASTER_EIO;
    struct fs_dirent ent;int result=0,read;
    while((read=fs_readdir(&dir,&ent))==0 && ent.name[0]) {
        if(!strcmp(ent.name,".") || !strcmp(ent.name,"..") ||
           strchr(ent.name,'/') || strchr(ent.name,'\\') || strchr(ent.name,':')) {result=ASTER_EINVAL;break;}
        char a[256],b[256];
        if(snprintf(a,sizeof(a),"%s/%s",src,ent.name)>=(int)sizeof(a) ||
           snprintf(b,sizeof(b),"%s/%s",dst,ent.name)>=(int)sizeof(b)){result=ASTER_EINVAL;break;}
        if(ent.type==FS_DIR_ENTRY_DIR)result=copy_bundle_tree(a,b,depth+1,count,bytes);
        else {
            (*count)++;*bytes+=ent.size;
            if(*count>4096 || *bytes>UINT64_C(1073741824)){result=ASTER_EMSGSIZE;break;}
            result=copy_bundle_file(a,b,ent.size);
        }
        if(result)break;
    }
    if(read<0)result=ASTER_EIO;
    if(fs_closedir(&dir))result=ASTER_EIO;
    return result;
}

static bool install_id_valid(const char *id)
{
	if (!id[0] || strstr(id, "..")) { return false; }
	for (size_t i = 0; id[i]; i++) {
		unsigned char c = (unsigned char)id[i];
		bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		if (!alnum && (i == 0 || (c != '.' && c != '_' && c != '-'))) { return false; }
	}
	return true;
}

#ifdef CONFIG_ASTER_PKG_FAULT_INJECTION
static void pkg_trace(const char *stage)
{
	struct k_thread *t = k_current_get();
	size_t unused = 0;
	(void)k_thread_stack_space_get(t, &unused);
	LOG_INF("pkg trace: %s thread=%p stack=%p/%u unused=%u", stage, t,
		(void *)t->stack_info.start, (unsigned)t->stack_info.size, (unsigned)unused);
}
#else
static void pkg_trace(const char *stage) { (void)stage; }
#endif

static int pkg_install_locked(const char *src_path,const char *resources,const char *caller)
{
	uint8_t *img = NULL;
	size_t size = 0;
	pkg_trace("load-source");
	int err = load_file(src_path, &img, &size);
	pkg_trace("loaded-source");
	if (err) {
		LOG_ERR("install: load %s (%d)", src_path, err);
		return ASTER_EIO;
	}

	aster_manifest_t m;
	const uint8_t *wasm; size_t wlen;
	const uint8_t *aot; size_t alen; uint32_t atarget;
	pkg_trace("parse-source");
	err = aster_pkg_parse(img, size, &m, &wasm, &wlen, &aot, &alen, &atarget);
	pkg_trace("parsed-source");
	if (err) {
		LOG_ERR("install: parse %s (%d)", src_path, err);
		aster_psram_free(img);
		return err;
	}

	if (!install_id_valid(m.id)) {
		LOG_ERR("install: stage=parse operation=app-id invalid APP identifier");
		aster_psram_free(img);
		return ASTER_ECORRUPT;
	}
    if(caller && !strcmp(caller,m.id)){aster_psram_free(img);return ASTER_EPERM;}

	/* A package carries exactly ONE AOT image, built for one target
	 * (pack_mja.py writes that id at header offset 36). Whether it matches
	 * this device is decided once, here -- the extraction below and the
	 * manifest's "aot" flag both read it, so they cannot disagree. */
	bool aot_ok = alen && atarget == CONFIG_ASTER_AOT_TARGET_ID;
	if (!aot_ok) {
#if !defined(ASTER_SYSTEM_ESP_IDF) || defined(ASTER_REQUIRE_AOT)
		LOG_ERR("install: %s requires AOT target %u (package target=%u, bytes=%u)",
			m.id, (unsigned)CONFIG_ASTER_AOT_TARGET_ID, atarget, (unsigned)alen);
		aster_psram_free(img);
		return alen ? ASTER_EABI : ASTER_ENOTSUP;
#endif
	}

	/* Signature gate BEFORE anything is written to the app dir. Policy is
	 * compile-time (docs/package-format.md); a tampered or alien-keyed package
	 * can never reach the launch path. */
	err = aster_sig_check(img, size);
	if (err) {
		LOG_ERR("install: signature %s (%d)", m.id[0] ? m.id : src_path, err);
		aster_psram_free(img);
		return err;
	}

#ifdef CONFIG_ASTER_PKG_FAULT_INJECTION
	snprintf(pkg_fault.active, sizeof(pkg_fault.active), "%s", m.id);
#endif
	/* No APP may retain a borrowed registry record across a version switch.
	 * The launch reservation and this check share files_lock. */
	if (aster_app_mgr_is_live(NULL)) {
		LOG_ERR("install: stage=prepare operation=busy app=%s; return to launcher first", m.id);
		aster_psram_free(img);
		return ASTER_EBUSY;
	}
	char base[128], appdir[128];
	aster_storage_app_dir(base, sizeof(base), m.id);
	struct aster_version current;
	pkg_trace("select-version");
	err = aster_versions_prepare(base, &current);
	pkg_trace("selected-version");
	if (err && err != -ENOENT) {
		LOG_ERR("install: stage=recovery operation=select path=%s result=%d", base, err);
		aster_psram_free(img);
		return ASTER_EIO;
	}
	unsigned slot = err || current.slot < 0 ? 0u : (unsigned)current.slot ^ 1u;
	uint32_t sequence = err ? 1u : current.sequence + 1u;
	/* Remove the inactive record BEFORE replacing its payload, so an old
	 * record cannot accidentally bind to a same-version rebuilt manifest. */
	char inactive_record[192];
	snprintf(inactive_record, sizeof(inactive_record), "%s/.active%u", base, slot);
	err = fs_unlink(inactive_record);
	if (err && err != -ENOENT) {
		LOG_ERR("install: stage=prepare operation=invalidate path=%s result=%d", inactive_record, err);
		aster_psram_free(img);
		return ASTER_EIO;
	}
	err = aster_versions_dir(appdir, sizeof(appdir), base, (int)slot);
	if (!err) { err = rmrf(appdir); }
	if (!err) { err = aster_storage_mkdir_p(appdir); }
	if (err) {
		LOG_ERR("install: stage=prepare operation=staging path=%s result=%d", base, err);
		aster_psram_free(img);
		return ASTER_EIO;
	}
	/* All writes go to the inactive slot. The old commit record and its
	 * payload are never modified, and user data/cache remain in base/. */
	char marker[192];
	snprintf(marker, sizeof(marker), "%s/%s", appdir, ASTER_INSTALL_MARKER);
	err = install_write(marker, "installing\n", 11, "begin");
	if (err) {
		aster_psram_free(img);
		return err;
	}

	/* Loaded package bytes may be in PSRAM; install_write always bounces. */
	char dst[192];
	snprintf(dst, sizeof(dst), "%s/package.ast", appdir);
	err = install_write(dst, img, size, "package");
	if (err) {
		aster_psram_free(img);
		return err;
	}
	snprintf(dst, sizeof(dst), "%s/app.wasm", appdir);
	err = install_write(dst, wasm, wlen, "wasm");
	if (err) {
		aster_psram_free(img);
		return err;
	}

	/* extract app.aot when the package's CPU matches the platform */
	if (aot_ok) {
		snprintf(dst, sizeof(dst), "%s/app.aot", appdir);
		err = install_write(dst, aot, alen, "aot");
		if (err) {
			aster_psram_free(img);
			return err;
		}
	}

	/* An absent/invalid optional icon keeps the existing fallback behavior;
	 * writing a valid supplied icon must succeed. */
	if (m.icon_len && aster_pkg_in_bounds(m.icon_off, m.icon_len, size, m.hdr_size) &&
	    aster_icon_parse(img + m.icon_off, m.icon_len, NULL) == ASTER_OK) {
		snprintf(dst, sizeof(dst), "%s/icon.bin", appdir);
		err = install_write(dst, img + m.icon_off, m.icon_len, "icon");
		if (err) {
			aster_psram_free(img);
			return err;
		}
	} else if (m.icon_len) {
		LOG_WRN("install: icon not ASTI, skip %s", m.id);
	}

	/* drop stale assets even when the new package has none */
	{
		char assets_dir[192];

		snprintf(assets_dir, sizeof(assets_dir), "%s/assets", appdir);
		err = rmrf(assets_dir);
		if (err) {
			LOG_ERR("install: stage=assets operation=cleanup path=%s result=%d", assets_dir, err);
			aster_psram_free(img);
			return ASTER_EIO;
		}
	}
	if (m.assets_len && aster_pkg_in_bounds(m.assets_off, m.assets_len, size, m.hdr_size)) {
		err = extract_mjr1(img + m.assets_off, m.assets_len, appdir);
		if (err) {
			LOG_ERR("install: stage=assets operation=extract path=%s result=%d", appdir, err);
			aster_psram_free(img);
			return err;
		}
	}

    if(resources) {
        char assets[192];unsigned count=0;uint64_t bytes=0;
        if(snprintf(assets,sizeof(assets),"%s/assets",appdir)>=(int)sizeof(assets))err=ASTER_EINVAL;
        else err=copy_bundle_tree(resources,assets,0,&count,&bytes);
        if(err){aster_psram_free(img);return err;}
    }
	/* Normalize the manifest.
	 *
	 * The whole line is composed AND validated before the file is created.
	 * An install that fails here used to leave a truncated manifest.json
	 * behind (no closing brace, or no arrays at all) while still reporting
	 * success -- and aster_app_mgr.c skips an unreadable manifest with a bare
	 * `continue`, so the app simply vanished from the launcher with nothing
	 * in the log. Overflow is therefore a hard ASTER_ECORRUPT, not a shorter
	 * manifest.
	 *
	 * How much room is there really? Measured against the vocabularies as
	 * they stand (sdk/tools/build.py VALID_CAPS/VALID_PERMS, the same words
	 * aster_json_str_array copies 8 at a time): the 8 longest capabilities
	 * cost 117 B per array, all four permissions cost 40 B, and the fixed
	 * prefix is 324 B even with id/name/version at the lengths their
	 * structs can hold -- 646 B for an app that fills every slot it can,
	 * plus up to 254 B of escaped description against 1152.
	 * The last real packages sit near 300 B.
	 *
	 * So the bound is not tight today, and that is exactly why the check
	 * below must stay: the ceiling is a property of the vocabularies, not
	 * of this buffer. One longer capability name moves it, and the failure
	 * it would cause is silent (see above), which is the whole reason
	 * overflowing is a hard ASTER_ECORRUPT rather than a shorter manifest. */
	/* Keep nested FS/CRC calls comfortably below the shell's stack budget. */
	char *line = aster_psram_alloc(ASTER_MANIFEST_MAX);
	if (!line) {
		LOG_ERR("install: stage=manifest operation=allocate bytes=%u", ASTER_MANIFEST_MAX);
		aster_psram_free(img);
		return ASTER_ENOMEM;
	}
	int w = snprintf(line, ASTER_MANIFEST_MAX,
		"{\"format\":1,\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\","
		"\"abi_min\":\"%s\",\"abi_target\":\"%s\",\"entry\":\"app.wasm\","
		"\"mem_initial\":%u,\"mem_max\":%u,\"mem_stack\":%u,\"mem_heap\":%u,"
		"\"aot\":%u,\"installed\":1",
		m.id, m.name, m.version, m.abi_min, m.abi_target,
		(unsigned)m.mem_initial, (unsigned)m.mem_max, (unsigned)(m.mem_stack ? m.mem_stack : 65536),
		(unsigned)(m.mem_heap ? m.mem_heap : 262144), aot_ok ? 1u : 0u);
	int r = (w > 0 && w < (int)ASTER_MANIFEST_MAX) ? 0 : -1;

	if (r >= 0 && m.description[0]) {
		r = json_string_field(line + w, ASTER_MANIFEST_MAX - w, "description", m.description);
		if (r >= 0) { w += r; r = 0; }
	}

	if (r >= 0) {
		r = json_arr(line + w, ASTER_MANIFEST_MAX - w, "requires",
			     m.requires, m.requires_n);
	}
	if (r >= 0) {
		w += r;
		r = json_arr(line + w, ASTER_MANIFEST_MAX - w, "optional",
			     m.optional, m.optional_n);
	}
	if (r >= 0) {
		w += r;
		r = json_arr(line + w, ASTER_MANIFEST_MAX - w, "permissions",
			     m.permissions, m.permissions_n);
	}
	if (r >= 0) {
		w += r;
	}
	if (r >= 0 && m.category[0]) {
		r = snprintf(line + w, ASTER_MANIFEST_MAX - w, ",\"category\":\"%s\"",
			     m.category);
		if (r < 0 || r >= (int)ASTER_MANIFEST_MAX - w) {
			r = -1;
		} else {
			w += r;
			r = 0;
		}
	}
	/* Written back so the key survives install: the registry reads THIS
	 * line, not the package. A value that was not understood is
	 * ASTER_ORIENT_ANY here and is omitted, which is the same thing the
	 * package would have meant by leaving the key out. */
	if (r >= 0 && aster_pkg_orientation_name(m.orientation) != NULL) {
		r = snprintf(line + w, ASTER_MANIFEST_MAX - w, ",\"orientation\":\"%s\"",
			     aster_pkg_orientation_name(m.orientation));
		if (r < 0 || r >= (int)ASTER_MANIFEST_MAX - w) {
			r = -1;
		} else {
			w += r;
			r = 0;
		}
	}
	if (r >= 0) {
		r = snprintf(line + w, ASTER_MANIFEST_MAX - w, "}\n");
		if (r < 0 || r >= (int)ASTER_MANIFEST_MAX - w) {
			r = -1;
		}
	}

	if (r < 0) {
		LOG_ERR("install %s: manifest needs more than %zu B "
			"(requires=%u optional=%u permissions=%u); refusing rather "
			"than writing a manifest the scanner would reject",
			m.id, (size_t)ASTER_MANIFEST_MAX, m.requires_n, m.optional_n,
			m.permissions_n);
		aster_psram_free(line);
		aster_psram_free(img);
		return ASTER_ECORRUPT;
	}
	w += r;

	snprintf(dst, sizeof(dst), "%s/manifest.json", appdir);
	err = install_write(dst, line, (size_t)w, "manifest");
	aster_psram_free(line);
	if (err) {
		aster_psram_free(img);
		return err;
	}

	snprintf(dst, sizeof(dst), "%s/data", base);
	err = aster_storage_mkdir_p(dst);
	if (err) {
		LOG_ERR("install: stage=data operation=mkdir path=%s result=%d", dst, err);
		aster_psram_free(img);
		return ASTER_EIO;
	}
	err = fs_unlink(marker);
	if (err) {
		LOG_ERR("install: stage=commit operation=remove-marker path=%s result=%d", marker, err);
		aster_psram_free(img);
		return ASTER_EIO;
	}

	uint32_t manifest_crc;
	err = aster_versions_manifest_crc(appdir, &manifest_crc);
	if (err) {
		LOG_ERR("install: stage=commit operation=seal path=%s result=%d", appdir, err);
		aster_psram_free(img);
		return ASTER_EIO;
	}
	uint8_t record[ASTER_VERSION_RECORD_SIZE];
	aster_versions_record(record, slot, sequence, manifest_crc);
	snprintf(dst, sizeof(dst), "%s/.active%u", base, slot);
	err = install_write(dst, record, sizeof(record), "commit");
	if (err) {
		LOG_ERR("install: stage=commit operation=publish path=%s result=%d", base, err);
		aster_psram_free(img);
		return err;
	}

	aster_psram_free(img);
	LOG_INF("installed %s (%s v%s, %u KB wasm)", m.id, m.name, m.version,
		(unsigned)(wlen / 1024));
	return ASTER_OK;
}

int aster_pkg_install_file(const char *src_path)
{
	aster_app_mgr_files_lock();
	int err = pkg_install_locked(src_path,NULL,NULL);
#ifdef CONFIG_ASTER_PKG_FAULT_INJECTION
	pkg_fault.active[0] = 0;
#endif
	aster_app_mgr_files_unlock();
	return err;
}

int aster_pkg_install_bundle_file(const char *src,const char *resources,const char *caller)
{
    aster_app_mgr_files_lock();
    int result=pkg_install_locked(src,resources,caller);
    aster_app_mgr_files_unlock();return result;
}
