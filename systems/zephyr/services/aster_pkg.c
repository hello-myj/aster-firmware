/* SPDX-License-Identifier: MIT */
#include "aster_pkg.h"
#include "aster_sig.h"
#include "aster_storage.h"
#include "aster_sysinfo.h"
#include "aster_mem.h"
#include "aster_bytes.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
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

static bool in_bounds(uint32_t off, size_t len, size_t total, size_t base)
{
	/* Not `(size_t)off + len <= total`: an app-supplied u32 offset near
	 * SIZE_MAX wraps that sum back into range, and every caller then
	 * reads img + off. Compare against the remainder instead, which
	 * cannot wrap once off itself is known to be in range. */
	const size_t o = off;

	return o >= base && o <= total && len <= total - o;
}

int aster_pkg_parse(const uint8_t *img, size_t size, aster_manifest_t *m,
		  const uint8_t **wasm, size_t *wasm_len,
		  const uint8_t **aot, size_t *aot_len, uint32_t *aot_target)
{
	if (size < ASTER_PKG_HDRSIZE) {
		return ASTER_ECORRUPT;
	}
	if (memcmp(img, ASTER_PKG_MAGIC, 4) != 0) {
		return ASTER_ECORRUPT;
	}
	uint16_t hdr_size = aster_le16(img + 6);
	if (aster_le16(img + 4) != 1 ||
	    (hdr_size != ASTER_PKG_HDRSIZE && hdr_size != ASTER_PKG_HDRSIZE_ASSETS) ||
	    size < hdr_size) {
		return ASTER_ECORRUPT;
	}

	uint32_t mo = aster_le32(img + 12), ml = aster_le32(img + 16);
	uint32_t wo = aster_le32(img + 20), wl = aster_le32(img + 24);
	uint32_t ao = aster_le32(img + 28), al = aster_le32(img + 32);
	uint32_t at = aster_le32(img + 36);
	uint32_t io = aster_le32(img + 40), il = aster_le32(img + 44);
	/* Bytes 56..63 are reserved: they held the ABI 1.5 ui.bin (UIB1) section
	 * offset/length, dropped in ABI 3.0 together with the widget runtime.
	 * Packages must write zeros; anything else is ignored, not rejected. */
	uint32_t aso = 0, asl = 0;

	if (hdr_size >= ASTER_PKG_HDRSIZE_ASSETS) {
		aso = aster_le32(img + 64);
		asl = aster_le32(img + 68);
	}

	if (!in_bounds(mo, ml, size, hdr_size) || ml == 0) {
		return ASTER_ECORRUPT;
	}
	if (!in_bounds(wo, wl, size, hdr_size) || wl < 8) {
		return ASTER_ECORRUPT;
	}
	/* wasm magic "\0asm" */
	if (memcmp(img + wo, "\0asm", 4) != 0) {
		return ASTER_ECORRUPT;
	}
	if (al) {
		if (!in_bounds(ao, al, size, hdr_size) || al < 4) {
			return ASTER_ECORRUPT;
		}
		if (memcmp(img + ao, "\0aot", 4) != 0) {
			return ASTER_ECORRUPT;
		}
	}
	if (il && !in_bounds(io, il, size, hdr_size)) {
		return ASTER_ECORRUPT;
	}
	if (asl) {
		if (!in_bounds(aso, asl, size, hdr_size) || asl < 16) {
			return ASTER_ECORRUPT;
		}
		if (memcmp(img + aso, "ASTR", 4) != 0) {
			return ASTER_ECORRUPT;
		}
	}

	/* manifest copy (NUL-terminated) */
	char *jbuf = k_malloc(ml + 1);
	if (!jbuf) {
		return ASTER_ENOMEM;
	}
	memcpy(jbuf, img + mo, ml);
	jbuf[ml] = 0;

	memset(m, 0, sizeof(*m));
	/* 0 is ASTER_ORIENT_PORTRAIT, so the zeroing above is NOT a valid default
	 * here: it would turn every package that never mentioned an orientation
	 * into one that asked for portrait. */
	m->orientation = ASTER_ORIENT_ANY;
	int err = 0;
	err |= aster_json_str(jbuf, "id", m->id, sizeof(m->id)) ? 0 : ASTER_ECORRUPT;
	err |= aster_json_str(jbuf, "name", m->name, sizeof(m->name)) ? 0 : ASTER_ECORRUPT;
	aster_json_str(jbuf, "version", m->version, sizeof(m->version));

	char abi[64];
	if (aster_json_obj(jbuf, "abi", abi, sizeof(abi)) == 0) {
		aster_json_str(abi, "min", m->abi_min, sizeof(m->abi_min));
		aster_json_str(abi, "target", m->abi_target, sizeof(m->abi_target));
	}

	char mem[128];
	if (aster_json_obj(jbuf, "memory", mem, sizeof(mem)) == 0) {
		uint32_t v;
		if (aster_json_u32(mem, "initial", &v) == 0) m->mem_initial = v;
		if (aster_json_u32(mem, "max", &v) == 0)     m->mem_max = v;
		if (aster_json_u32(mem, "stack", &v) == 0)   m->mem_stack = v;
		if (aster_json_u32(mem, "heap", &v) == 0)    m->mem_heap = v;
	}

	m->requires_n = aster_json_str_array(jbuf, "requires", m->requires, 8);
	m->optional_n = aster_json_str_array(jbuf, "optional", m->optional, 8);
	m->permissions_n = aster_json_str_array(jbuf, "permissions", m->permissions, 8);
	{
		char cat[16];

		if (aster_json_str(jbuf, "category", cat, sizeof(cat)) >= 0) {
			aster_pkg_norm_category(cat, m->category, sizeof(m->category));
		}
	}
	{
		char orient[16];

		if (aster_json_str(jbuf, "orientation", orient,
				 sizeof(orient)) >= 0) {
			m->orientation = aster_pkg_norm_orientation(orient);
			if (m->orientation == ASTER_ORIENT_ANY) {
				/* Drop the key rather than guess a default: a
				 * package that asked to be pinned and misspelled
				 * it must not quietly get the other orientation,
				 * and this is the only moment anyone can be told
				 * which package it was. */
				LOG_WRN("manifest: orientation '%s' is not "
					"portrait/landscape; ignored, app follows "
					"the panel", orient);
			}
		}
	}

    /* Aster package identity is mandatory, including interpreter-only packages. */
    char product[16] = {0}, target_id[64] = {0};
    aster_json_str(jbuf, "product", product, sizeof(product));
    aster_json_str(jbuf, "target_id", target_id, sizeof(target_id));
    if (strcmp(product, "aster") != 0 || strcmp(target_id, CONFIG_ASTER_TARGET_ID) != 0) {
        LOG_ERR("package product/target mismatch: %s/%s", product, target_id);
        k_free(jbuf);
        return ASTER_EABI;
    }
	k_free(jbuf);

	if (m->id[0] == 0) {
		return ASTER_ECORRUPT;
	}

	/* ABI compatibility: device major must equal app major, and the device
	 * minor must be >= the app's declared abi_min. An app that hard-imports
	 * a newer native symbol (e.g. the appmgr family) must bump abi_min, so
	 * older firmware rejects at install instead of trapping at launch. */
	uint32_t dev = ASTER_ABI_VERSION;
	uint32_t appmajor = 1, appminor = 0;
	if (m->abi_min[0]) {
		appmajor = (uint32_t)atoi(m->abi_min);
		const char *dot = strchr(m->abi_min, '.');
		if (dot) {
			appminor = (uint32_t)atoi(dot + 1);
		}
	}
	if ((dev >> 16) != appmajor || (((dev >> 8) & 0xFFu) < appminor)) {
		LOG_ERR("abi mismatch: device %u.%u app min %u.%u",
			dev >> 16, (dev >> 8) & 0xFFu, appmajor, appminor);
		return ASTER_EABI;
	}

	/* requires capability check */
	for (uint32_t i = 0; i < m->requires_n; i++) {
		if (!aster_sysinfo_feature_by_name(m->requires[i])) {
			LOG_ERR("missing required capability: %s", m->requires[i]);
			return ASTER_ENOTSUP;
		}
	}

	if (wasm) {
		*wasm = img + wo;
	}
	if (wasm_len) {
		*wasm_len = wl;
	}
	if (aot) {
		*aot = al ? img + ao : NULL;
	}
	if (aot_len) {
		*aot_len = al;
	}
	if (aot_target) {
		*aot_target = at;
	}
	if (m) {
		m->has_icon = il != 0;
		m->icon_off = io;
		m->icon_len = il;
		m->icon_w = aster_le16(img + 52);
		m->icon_h = aster_le16(img + 54);
		m->hdr_size = hdr_size;
		m->assets_off = aso;
		m->assets_len = asl;
	}
	return ASTER_OK;
}

int8_t aster_pkg_norm_orientation(const char *src)
{
	if (!src || !src[0]) {
		return ASTER_ORIENT_ANY;
	}
	if (strcmp(src, "portrait") == 0) {
		return (int8_t)ASTER_ORIENT_PORTRAIT;
	}
	if (strcmp(src, "landscape") == 0) {
		return (int8_t)ASTER_ORIENT_LANDSCAPE;
	}
	return ASTER_ORIENT_ANY;
}

const char *aster_pkg_orientation_name(int8_t o)
{
	/* The two spellings are shared with aster_pkg_norm_orientation above and
	 * with sdk/tools/{build,verify_mja}.py. A third state ("any") has no
	 * spelling on purpose: see ASTER_ORIENT_ANY in the header. */
	switch (o) {
	case (int8_t)ASTER_ORIENT_PORTRAIT:
		return "portrait";
	case (int8_t)ASTER_ORIENT_LANDSCAPE:
		return "landscape";
	default:
		return NULL;
	}
}

void aster_pkg_norm_category(const char *src, char *dst, size_t n)
{
	if (!dst || n == 0) {
		return;
	}
	dst[0] = 0;
	if (!src || !src[0]) {
		return;
	}
	if (strcmp(src, "game") != 0 && strcmp(src, "tool") != 0 &&
	    strcmp(src, "system") != 0) {
		return;
	}
	strncpy(dst, src, n - 1);
	dst[n - 1] = 0;
}

/* ---- minimal JSON helpers ---- */

static const char *find_key(const char *json, const char *key)
{
	char pat[40];
	if (snprintf(pat, sizeof(pat), "\"%s\"", key) >= (int)sizeof(pat)) {
		return NULL;
	}
	const char *p = json;
	while ((p = strstr(p, pat)) != NULL) {
		const char *q = p + strlen(pat);
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
			q++;
		}
		if (*q == ':') {
			return q + 1;
		}
		p = q;
	}
	return NULL;
}

int aster_json_str(const char *json, const char *key, char *out, size_t n)
{
	const char *v = find_key(json, key);
	if (!v) {
		return -1;
	}
	while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') {
		v++;
	}
	if (*v != '"') {
		return -1;
	}
	v++;
	size_t i = 0;
	while (*v && *v != '"' && i + 1 < n) {
		if (*v == '\\' && v[1]) {
			v++;
		}
		out[i++] = *v++;
	}
	out[i] = 0;
	return (int)i;
}

int aster_json_u32(const char *json, const char *key, uint32_t *out)
{
	const char *v = find_key(json, key);
	if (!v) {
		return -1;
	}
	while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') {
		v++;
	}
	char *end;
	unsigned long x = strtoul(v, &end, 10);
	if (end == v) {
		return -1;
	}
	*out = (uint32_t)x;
	return 0;
}

int aster_json_obj(const char *json, const char *key, char *out, size_t n)
{
	const char *v = find_key(json, key);
	if (!v) {
		return -1;
	}
	while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') {
		v++;
	}
	if (*v != '{') {
		return -1;
	}
	const char *end = strchr(v, '}');
	if (!end) {
		return -1;
	}
	size_t len = (size_t)(end - v + 1);
	if (len >= n) {
		len = n - 1;
	}
	memcpy(out, v, len);
	out[len] = 0;
	return 0;
}

int aster_json_str_array(const char *json, const char *key, char arr[][24], int max)
{
	const char *v = find_key(json, key);
	if (!v) {
		return 0;
	}
	while (*v && *v != '[') {
		v++;
	}
	if (*v != '[') {
		return 0;
	}
	v++;
	int n = 0;
	while (*v && *v != ']' && n < max) {
		while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r' || *v == ',') {
			v++;
		}
		if (*v == ']') {
			break;
		}
		if (*v != '"') {
			v++;
			continue;
		}
		v++;
		int i = 0;
		while (*v && *v != '"' && i < 23) {
			arr[n][i++] = *v++;
		}
		arr[n][i] = 0;
		if (*v == '"') {
			v++;
		}
		n++;
	}
	return n;
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

static int rmrf(const char *path)
{
	struct fs_dir_t dir;
	struct fs_dirent st;
	int err = fs_stat(path, &st);

	if (err) {
		return 0;
	}
	if (st.type != FS_DIR_ENTRY_DIR) {
		return fs_unlink(path);
	}
	fs_dir_t_init(&dir);
	err = fs_opendir(&dir, path);
	if (err) {
		return err;
	}
	while (fs_readdir(&dir, &st) == 0 && st.name[0]) {
		char child[256];
		size_t plen = strlen(path);
		size_t nlen = strlen(st.name);

		if (plen + 1 + nlen >= sizeof(child)) {
			continue;
		}
		memcpy(child, path, plen);
		child[plen] = '/';
		memcpy(child + plen + 1, st.name, nlen + 1);
		if (st.type == FS_DIR_ENTRY_DIR) {
			rmrf(child);
		} else {
			fs_unlink(child);
		}
	}
	fs_closedir(&dir);
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
	(void)rmrf(root);
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
		struct fs_file_t f;
		int err;
		size_t max_rel;

		if (po >= len || fo > len || fl > len || fo + fl > len) {
			return ASTER_ECORRUPT;
		}
		rel = (const char *)(blob + po);
		max_rel = len - po;
		if (max_rel == 0 || !memchr(rel, 0, max_rel) || !asset_relpath_ok(rel)) {
			LOG_WRN("install: skip bad asset path");
			continue;
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
		fs_file_t_init(&f);
		err = fs_open(&f, dest, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
		if (err) {
			LOG_ERR("install: open %s (%d)", dest, err);
			return ASTER_EIO;
		}
		err = aster_fs_write_safe(&f, blob + fo, fl);
		fs_close(&f);
		if (err < (int)fl) {
			LOG_ERR("install: write %s (%d/%u)", dest, err, (unsigned)fl);
			return ASTER_EIO;
		}
	}
	LOG_INF("install: assets %u file(s) -> %s", n, root);
	return ASTER_OK;
}

int aster_pkg_install_file(const char *src_path)
{
	uint8_t *img = NULL;
	size_t size = 0;
	int err = load_file(src_path, &img, &size);
	if (err) {
		LOG_ERR("install: load %s (%d)", src_path, err);
		return ASTER_EIO;
	}

	aster_manifest_t m;
	const uint8_t *wasm; size_t wlen;
	const uint8_t *aot; size_t alen; uint32_t atarget;
	err = aster_pkg_parse(img, size, &m, &wasm, &wlen, &aot, &alen, &atarget);
	if (err) {
		LOG_ERR("install: parse %s (%d)", src_path, err);
		aster_psram_free(img);
		return err;
	}

	/* A package carries exactly ONE AOT image, built for one target
	 * (pack_mja.py writes that id at header offset 36). Whether it matches
	 * this device is decided once, here -- the extraction below and the
	 * manifest's "aot" flag both read it, so they cannot disagree. */
	bool aot_ok = alen && atarget == CONFIG_ASTER_AOT_TARGET_ID;

	/* Signature gate BEFORE anything is written to the app dir. Policy is
	 * compile-time (docs/package-format.md); a tampered or alien-keyed package
	 * can never reach the launch path. */
	err = aster_sig_check(img, size);
	if (err) {
		LOG_ERR("install: signature %s (%d)", m.id[0] ? m.id : src_path, err);
		aster_psram_free(img);
		return err;
	}

	char appdir[128];
	aster_storage_app_dir(appdir, sizeof(appdir), m.id);

	/* make sure the per-app dir tree exists (fresh card / lost ensure_dirs) */
	int derr = aster_storage_mkdir_p(appdir);
	if (derr) {
		LOG_ERR("install: mkdir %s (%d)", appdir, derr);
		aster_psram_free(img);
		return ASTER_EIO;
	}

	/* fresh install: wipe old package files but keep data/ */
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, appdir) == 0) {
		struct fs_dirent ent;
		while (fs_readdir(&dir, &ent) == 0 && ent.name[0]) {
			if (ent.type != FS_DIR_ENTRY_FILE) {
				continue;
			}
			/* keep data/ and cache/ dirs (handled above), delete files */
			char p[192];
			size_t plen = strlen(appdir), nlen = strlen(ent.name);

			if (plen + 1 + nlen >= sizeof(p)) {
				continue;
			}
			memcpy(p, appdir, plen);
			p[plen] = '/';
			memcpy(p + plen + 1, ent.name, nlen + 1);
			fs_unlink(p);
		}
		fs_closedir(&dir);
	}

	/* copy package.ast -- SOURCE IS PSRAM: the SD controller's DMA cannot
	 * read external RAM, a raw fs_write here lands 0-byte or partially
	 * corrupt files on disk (observed: app.aot 0 B -> silent interpreter
	 * fallback at ~2 fps; "主" glyph byte-hole in app.wasm -> tofu).
	 * aster_fs_write_safe shuttles through an internal-RAM bounce. */
	char dst[192];
	snprintf(dst, sizeof(dst), "%s/package.ast", appdir);
	struct fs_file_t f;
	fs_file_t_init(&f);
	err = fs_open(&f, dst, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
	if (err) {
		LOG_ERR("install: open %s (%d)", dst, err);
		aster_psram_free(img);
		return ASTER_EIO;
	}
	err = aster_fs_write_safe(&f, img, size);
	fs_close(&f);
	if (err < (int)size) {
		LOG_ERR("install: write %s (%d/%u)", dst, err, (unsigned)size);
		aster_psram_free(img);
		return ASTER_EIO;
	}

	/* extract app.wasm */
	snprintf(dst, sizeof(dst), "%s/app.wasm", appdir);
	fs_file_t_init(&f);
	err = fs_open(&f, dst, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
	if (err) {
		aster_psram_free(img);
		return ASTER_EIO;
	}
	err = aster_fs_write_safe(&f, wasm, wlen);
	fs_close(&f);
	if (err < (int)wlen) {
		LOG_ERR("install: write %s (%d/%u)", dst, err, (unsigned)wlen);
		aster_psram_free(img);
		return ASTER_EIO;
	}

	/* extract app.aot when the package's AOT target matches this platform's
	 * (CONFIG_ASTER_AOT_TARGET_ID is derived from the CPU arch chosen by the
	 * board port) */
	if (aot_ok) {
		snprintf(dst, sizeof(dst), "%s/app.aot", appdir);
		fs_file_t_init(&f);
		err = fs_open(&f, dst, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
		if (!err) {
			err = aster_fs_write_safe(&f, aot, alen);
			fs_close(&f);
			if (err < (int)alen) {
				/* not fatal: wasm fallback still runs, but say so
				 * loudly -- this exact silent failure cost 20x fps */
				LOG_WRN("install: aot write %s (%d/%u), interpreter fallback",
					dst, err, (unsigned)alen);
			}
		}
	} else if (alen) {
		/* The package HAS an AOT image, for the wrong CPU. It installs
		 * and runs, at roughly 1/20th the frame rate. This used to
		 * happen with no log line at all -- the app just got slow, and
		 * the only way to find out why was to hexdump the package
		 * header against the board config. */
		LOG_ERR("install: %s: package AOT target %u, this device runs %u "
			"-- aot DROPPED, app will run interpreted (much slower); "
			"rebuild the package for this device's AOT target",
			m.id, atarget, (unsigned)CONFIG_ASTER_AOT_TARGET_ID);
	}

	/* launcher tile: ASTI at /SD:/aster/apps/<id>/icon.bin */
	snprintf(dst, sizeof(dst), "%s/icon.bin", appdir);
	if (m.icon_len && in_bounds(m.icon_off, m.icon_len, size, m.hdr_size) &&
	    aster_icon_parse(img + m.icon_off, m.icon_len, NULL) == ASTER_OK) {
		fs_file_t_init(&f);
		err = fs_open(&f, dst, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
		if (!err) {
			err = aster_fs_write_safe(&f, img + m.icon_off, m.icon_len);
			fs_close(&f);
			if (err < (int)m.icon_len) {
				LOG_WRN("install: icon.bin write %s (%d/%u)", dst, err,
					(unsigned)m.icon_len);
				(void)fs_unlink(dst);
			}
		}
	} else {
		struct fs_dirent st;

		if (fs_stat(dst, &st) == 0) {
			(void)fs_unlink(dst);
		}
		if (m.icon_len) {
			LOG_WRN("install: icon not ASTI, skip %s", m.id);
		}
	}

	/* drop stale assets even when the new package has none */
	{
		char assets_dir[192];

		snprintf(assets_dir, sizeof(assets_dir), "%s/assets", appdir);
		(void)rmrf(assets_dir);
	}
	if (m.assets_len && in_bounds(m.assets_off, m.assets_len, size, m.hdr_size)) {
		err = extract_mjr1(img + m.assets_off, m.assets_len, appdir);
		if (err) {
			LOG_ERR("install: assets %s (%d)", m.id, err);
			aster_psram_free(img);
			return err;
		}
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
	 * against 832. The last real packages sit near 300 B.
	 *
	 * So the bound is not tight today, and that is exactly why the check
	 * below must stay: the ceiling is a property of the vocabularies, not
	 * of this buffer. One longer capability name moves it, and the failure
	 * it would cause is silent (see above), which is the whole reason
	 * overflowing is a hard ASTER_ECORRUPT rather than a shorter manifest. */
	char line[ASTER_MANIFEST_MAX];
	int w = snprintf(line, sizeof(line),
		"{\"format\":1,\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\","
		"\"abi_min\":\"%s\",\"abi_target\":\"%s\",\"entry\":\"app.wasm\","
		"\"mem_initial\":%u,\"mem_max\":%u,\"mem_stack\":%u,\"mem_heap\":%u,"
		"\"aot\":%u,\"installed\":1",
		m.id, m.name, m.version, m.abi_min, m.abi_target,
		m.mem_initial, m.mem_max, m.mem_stack ? m.mem_stack : 65536,
		m.mem_heap ? m.mem_heap : 262144, aot_ok ? 1u : 0u);
	int r = (w > 0 && w < (int)sizeof(line)) ? 0 : -1;

	/* Record a dropped AOT so the reason outlives the install log. Written
	 * only when there is something to skip, so every package that matched
	 * (the ordinary case) produces the exact byte string it always did.
	 * The value is the package's target id, not a boolean: "why is this
	 * app at 2 fps" is answered by which target it wanted. */
	if (r >= 0 && alen && !aot_ok) {
		r = snprintf(line + w, sizeof(line) - w, ",\"aot_skip\":%u", atarget);
		if (r < 0 || r >= (int)sizeof(line) - w) {
			r = -1;
		} else {
			w += r;
			r = 0;
		}
	}

	if (r >= 0) {
		r = json_arr(line + w, sizeof(line) - w, "requires",
			     m.requires, m.requires_n);
	}
	if (r >= 0) {
		w += r;
		r = json_arr(line + w, sizeof(line) - w, "optional",
			     m.optional, m.optional_n);
	}
	if (r >= 0) {
		w += r;
		r = json_arr(line + w, sizeof(line) - w, "permissions",
			     m.permissions, m.permissions_n);
	}
	if (r >= 0) {
		w += r;
	}
	if (r >= 0 && m.category[0]) {
		r = snprintf(line + w, sizeof(line) - w, ",\"category\":\"%s\"",
			     m.category);
		if (r < 0 || r >= (int)sizeof(line) - w) {
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
		r = snprintf(line + w, sizeof(line) - w, ",\"orientation\":\"%s\"",
			     aster_pkg_orientation_name(m.orientation));
		if (r < 0 || r >= (int)sizeof(line) - w) {
			r = -1;
		} else {
			w += r;
			r = 0;
		}
	}
	if (r >= 0) {
		r = snprintf(line + w, sizeof(line) - w, "}\n");
		if (r < 0 || r >= (int)sizeof(line) - w) {
			r = -1;
		}
	}

	if (r < 0) {
		LOG_ERR("install %s: manifest needs more than %zu B "
			"(requires=%u optional=%u permissions=%u); refusing rather "
			"than writing a manifest the scanner would reject",
			m.id, sizeof(line), m.requires_n, m.optional_n,
			m.permissions_n);
		aster_psram_free(img);
		return ASTER_ECORRUPT;
	}
	w += r;

	snprintf(dst, sizeof(dst), "%s/manifest.json", appdir);
	fs_file_t_init(&f);
	err = fs_open(&f, dst, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
	if (err == 0) {
		fs_write(&f, line, (size_t)w);
		fs_close(&f);
	}

	/* data dir */
	snprintf(dst, sizeof(dst), "%s/data", appdir);
	aster_storage_mkdir_p(dst);

	aster_psram_free(img);
	LOG_INF("installed %s (%s v%s, %u KB wasm)", m.id, m.name, m.version,
		(unsigned)(wlen / 1024));
	return ASTER_OK;
}
