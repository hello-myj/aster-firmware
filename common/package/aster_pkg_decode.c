/* SPDX-License-Identifier: MIT */
#include "aster_pkg_format.h"
#include "aster_bytes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
static void report(const aster_pkg_policy_t *policy, bool error, const char *format, ...)
{
    if (!policy->diagnostic) return;
    char message[192];va_list args;va_start(args,format);
    vsnprintf(message,sizeof(message),format,args);va_end(args);
    policy->diagnostic(error,message);
}
bool aster_pkg_in_bounds(uint32_t off, size_t len, size_t total, size_t base)
{
	/* Not `(size_t)off + len <= total`: an app-supplied u32 offset near
	 * SIZE_MAX wraps that sum back into range, and every caller then
	 * reads img + off. Compare against the remainder instead, which
	 * cannot wrap once off itself is known to be in range. */
	const size_t o = off;

	return o >= base && o <= total && len <= total - o;
}

int aster_pkg_decode(const aster_pkg_policy_t *policy, const uint8_t *img, size_t size, aster_manifest_t *m,
		  const uint8_t **wasm, size_t *wasm_len,
		  const uint8_t **aot, size_t *aot_len, uint32_t *aot_target)
{
	if (!policy || !policy->target_id || !policy->alloc || !policy->release || !policy->has_feature || !img || !m) return ASTER_EINVAL;
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

	if (!aster_pkg_in_bounds(mo, ml, size, hdr_size) || ml == 0) {
		return ASTER_ECORRUPT;
	}
	if (!aster_pkg_in_bounds(wo, wl, size, hdr_size) || wl < 8) {
		return ASTER_ECORRUPT;
	}
	/* wasm magic "\0asm" */
	if (memcmp(img + wo, "\0asm", 4) != 0) {
		return ASTER_ECORRUPT;
	}
	if (al) {
		if (!aster_pkg_in_bounds(ao, al, size, hdr_size) || al < 4) {
			return ASTER_ECORRUPT;
		}
		if (memcmp(img + ao, "\0aot", 4) != 0) {
			return ASTER_ECORRUPT;
		}
	}
	if (il && !aster_pkg_in_bounds(io, il, size, hdr_size)) {
		return ASTER_ECORRUPT;
	}
	if (asl) {
		if (!aster_pkg_in_bounds(aso, asl, size, hdr_size) || asl < 16) {
			return ASTER_ECORRUPT;
		}
		if (memcmp(img + aso, "ASTR", 4) != 0) {
			return ASTER_ECORRUPT;
		}
	}

	/* manifest copy (NUL-terminated) */
	char *jbuf = policy->alloc(ml + 1);
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
	aster_pkg_read_description(jbuf, m->description);
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
				report(policy, false, "manifest: orientation '%s' is not "
					"portrait/landscape; ignored, app follows "
					"the panel", orient);
			}
		}
	}

    /* Aster package identity is mandatory, including interpreter-only packages. */
    char product[16] = {0}, target_id[64] = {0};
    aster_json_str(jbuf, "product", product, sizeof(product));
    aster_json_str(jbuf, "target_id", target_id, sizeof(target_id));
    if (strcmp(product, "aster") != 0 || strcmp(target_id, policy->target_id) != 0) {
        report(policy, true, "package product/target mismatch: %s/%s", product, target_id);
        policy->release(jbuf);
        return ASTER_EABI;
    }
	policy->release(jbuf);

	if (m->id[0] == 0) {
		return ASTER_ECORRUPT;
	}

	/* ABI compatibility: device major must equal app major, and the device
	 * minor must be >= the app's declared abi_min. An app that hard-imports
	 * a newer native symbol (e.g. the appmgr family) must bump abi_min, so
	 * older firmware rejects at install instead of trapping at launch. */
	uint32_t dev = policy->abi_version;
	uint32_t appmajor = 1, appminor = 0;
	if (m->abi_min[0]) {
		appmajor = (uint32_t)atoi(m->abi_min);
		const char *dot = strchr(m->abi_min, '.');
		if (dot) {
			appminor = (uint32_t)atoi(dot + 1);
		}
	}
	if ((dev >> 16) != appmajor || (((dev >> 8) & 0xFFu) < appminor)) {
		report(policy, true, "abi mismatch: 需用新 SDK 重编 (device %u.%u app min %u.%u)",
			dev >> 16, (dev >> 8) & 0xFFu, appmajor, appminor);
		return ASTER_EABI;
	}

	/* requires capability check */
	for (uint32_t i = 0; i < m->requires_n; i++) {
		if (!policy->has_feature(m->requires[i])) {
			report(policy, true, "missing required capability: %s", m->requires[i]);
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

/* Old packages have no introduction. Own the text and never leave a
 * partial UTF-8 character when a third-party package exceeds the limit. */
void aster_pkg_read_description(const char *json, char *out)
{
	out[0] = 0;
	if (aster_json_str(json, "description", out, ASTER_APP_DESCRIPTION_MAX) < 0) {
		return;
	}
	size_t len = strlen(out);
	if (!len) return;
	size_t start = len - 1;
	while (start && ((unsigned char)out[start] & 0xc0) == 0x80) start--;
	unsigned char c = (unsigned char)out[start];
	size_t width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
	if (len - start < width) out[start] = 0;
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
