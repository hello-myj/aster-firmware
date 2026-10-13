/* SPDX-License-Identifier: MIT
 * .ast package parser (docs/package-format.md).
 */
#ifndef ASTER_PKG_H
#define ASTER_PKG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <aster/error.h>
#include <aster/display.h>

#define ASTER_PKG_MAGIC   "ASTP"
#define ASTER_PKG_HDRSIZE 64
#define ASTER_PKG_HDRSIZE_ASSETS 72

/* Capacity of the NORMALIZED manifest that install writes and the registry
 * reads. One number, because three separate buffers have to agree on it and
 * used not to: install composes the line, app_mgr's scan reads it back, and
 * app_mgr's read_manifest reads it again. A reader smaller than the writer
 * turns overflow from an error into a disappearance -- a short scan buffer
 * drops the app from the launcher, a short read_manifest buffer reports an app
 * that IS listed as ASTER_ENOENT. Fix overflow here, never at a call site. */
#define ASTER_MANIFEST_MAX 1152
/* Directories with this durable marker are never treated as installed APPs. */
#define ASTER_INSTALL_MARKER ".installing"
/* Optional launcher introduction, at most 127 UTF-8 bytes plus NUL. */
#define ASTER_APP_DESCRIPTION_MAX 128

#define ASTER_PKG_FLAG_SIGNED  1u
/* bit1 reserved: was ASTER_PKG_FLAG_HAS_UI (UIB1 ui.bin), dropped in ABI 3.0 */
#define ASTER_PKG_FLAG_ASSETS  4u

#define ASTER_AOT_TARGET_NONE      0u
#define ASTER_AOT_TARGET_XTENSA    1u
#define ASTER_AOT_TARGET_RISCV     2u
#define ASTER_AOT_TARGET_THUMB_V7  3u
#define ASTER_AOT_TARGET_CORTEX_M  4u
#define ASTER_AOT_TARGET_X86_64    5u

/* The orientation a package asks for, from the manifest's "orientation" key.
 * The two real values are the display ABI's ASTER_ORIENT_x, so there is one
 * numeric vocabulary rather than a manifest-private copy of it.
 *
 * ASTER_ORIENT_ANY is "the package asks for nothing" -- the app takes whatever
 * the panel is. That is also what an ABSENT key means, and a manifest cannot
 * spell it: absence is the only form. Deliberate, because the alternative
 * ("any"/"auto"/"default" all being valid) is a vocabulary in which a typo
 * silently becomes a request. A misspelled value is dropped with a warning
 * instead (aster_pkg_norm_orientation) -- an app that asked to be pinned must
 * not quietly get the other orientation.
 *
 * NOTE: 0 is ASTER_ORIENT_PORTRAIT, so a struct that was memset to zero does
 * NOT mean "adaptive". Every producer has to set this explicitly. */
#define ASTER_ORIENT_ANY ((int8_t)-1)

/* Manifest fields kept after parsing */
typedef struct {
	char     id[64];
	char     name[40];
	char     description[ASTER_APP_DESCRIPTION_MAX];
	char     version[24];
	char     abi_min[8];
	char     abi_target[8];
	uint32_t mem_initial;
	uint32_t mem_max;
	uint32_t mem_stack;
	uint32_t mem_heap;
	char     requires[8][24];
	uint32_t requires_n;
	char     optional[8][24];
	uint32_t optional_n;
	char     permissions[8][24];
	uint32_t permissions_n;
	char     category[16]; /* game / tool / system / empty */
	int8_t   orientation;  /* ASTER_ORIENT_x, or ASTER_ORIENT_ANY */
	bool     has_icon;
	uint32_t icon_off;
	uint32_t icon_len;
	uint32_t icon_w;
	uint32_t icon_h;
	uint16_t hdr_size;
	uint32_t assets_off;
	uint32_t assets_len;
} aster_manifest_t;

/* Parse + validate header from an in-memory image. Sections returned. */
int aster_pkg_parse(const uint8_t *img, size_t size, aster_manifest_t *m,
		  const uint8_t **wasm, size_t *wasm_len,
		  const uint8_t **aot, size_t *aot_len, uint32_t *aot_target);

/* Install a package file into /SD:/aster/apps/<id>. Validates ABI + caps. */
int aster_pkg_install_file(const char *src_path);

#ifdef CONFIG_ASTER_PKG_FAULT_INJECTION
int aster_pkg_fault_arm(const char *id, const char *stage, const char *operation,
                        unsigned action);
void aster_pkg_fault_clear(void);
#endif

/* Minimal JSON helpers (objects are flat-ish; nested one level allowed) */
int aster_json_str(const char *json, const char *key, char *out, size_t n);
void aster_pkg_read_description(const char *json, char *out);
int aster_json_u32(const char *json, const char *key, uint32_t *out);
int aster_json_str_array(const char *json, const char *key, char arr[][24], int max);
/* extract a nested object body into out (caller-owned buf) */
int aster_json_obj(const char *json, const char *key, char *out, size_t n);
/* Copy src if it is game/tool/system; otherwise empty. */
void aster_pkg_norm_category(const char *src, char *dst, size_t n);
/* "portrait"/"landscape" -> ASTER_ORIENT_x; anything else, including no key at
 * all, -> ASTER_ORIENT_ANY. Callers that had a KEY and got ASTER_ORIENT_ANY back
 * should log it: that is a package asking for something this build does not
 * understand, which is worth saying out loud once at install time. */
int8_t aster_pkg_norm_orientation(const char *src);
/* ASTER_ORIENT_x -> the manifest spelling, ASTER_ORIENT_ANY -> NULL. The inverse
 * of the above, so the string vocabulary exists in exactly one file. */
const char *aster_pkg_orientation_name(int8_t o);

#endif
