/* SPDX-License-Identifier: MIT
 * App Manager: registry, lifecycle, install/uninstall (design doc sec.45).
 * The runtime calls aster_app_mgr_set_state() to reflect transitions.
 */
#ifndef ASTER_APP_MGR_H
#define ASTER_APP_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "aster_pkg.h"

typedef enum {
	ASTER_APP_STATE_STOPPED = 0,
	ASTER_APP_STATE_RUNNING,
	ASTER_APP_STATE_PAUSED,
	ASTER_APP_STATE_CRASHED,
} aster_app_state_t;

typedef struct {
	char            id[64];
	char            name[40];
	char            description[ASTER_APP_DESCRIPTION_MAX];
	char            version[24];
	char            entry[48];   /* "app.wasm" or "app.aot" */
	uint32_t        abi_major;
	char            abi_min[8];
	uint32_t        mem_initial;
	uint32_t        mem_max;
	uint32_t        mem_stack;
	uint32_t        mem_heap;
	bool            has_aot;
	/* Package carried an AOT image for a DIFFERENT target, so install
	 * dropped it and the app runs interpreted. Holds that target id
	 * (ASTER_AOT_TARGET_*), 0 when there was nothing to skip. Without this
	 * the drop is invisible after the install log scrolls away: has_aot
	 * is merely false, which is also what a wasm-only package looks
	 * like.
	 *
	 * 32 bits, like the id it holds: this was a uint8_t, and the scan's
	 * cast made target 999 come back out of `aster app info` as 231 -- the
	 * report said "rebuild for 231", an id no package has. The manifest
	 * on disk was right all along; the field was the liar. */
	uint32_t        aot_skip;
	bool            has_icon;   /* install dir has icon.bin (ASTI) */
	char            category[16]; /* game / tool / system / empty */
	/* Orientation the package asked for (ASTER_ORIENT_x), or ASTER_ORIENT_ANY
	 * when it asked for none. NOT zero-initialisable: 0 is portrait, so a
	 * memset'd record would claim every app wants portrait. Scan sets this
	 * explicitly on every pass, which also means changing the manifest
	 * takes effect on the next scan rather than at the next boot. */
	int8_t          want_orientation;
	uint32_t        perm_mask;  /* ASTER_PERM_x (from manifest permissions[]) */
	aster_app_state_t state;
	uint32_t        last_error;
	uint32_t        start_count;
	uint64_t        installed_at;
	uint64_t        started_at;
	uint32_t        peak_mem;    /* wasm linear memory bytes */
} aster_app_rec_t;

#define ASTER_APP_MAX_RECS 16

int  aster_app_mgr_init(void);
/* rescan /SD:/aster/apps. The runtime-owned fields (state, start counters) of
 * the app that is currently running survive the rescan: it must not be
 * reposted as STOPPED, or uninstall could delete a live app's directory. */
int  aster_app_mgr_scan(void);
int  aster_app_mgr_count(void);
/* Borrowed view of a record in the fixed recs[] array. The pointer is only
 * valid until the next scan()/install()/uninstall(): scan rewrites every
 * record and uninstall compacts the array, so no slot is pinned. Copy the
 * fields you need before such a call. (aster_runtime.c instead keeps
 * ctx->rec for the lifetime of the app it runs -- that is why a registry
 * change must never target a live record.) */
const aster_app_rec_t *aster_app_mgr_get(int idx);
const aster_app_rec_t *aster_app_mgr_find(const char *id);
int  aster_app_mgr_install(const char *src_path);
int  aster_app_mgr_uninstall(const char *id);
/* read manifest.json fields into rec (used by scan) */
int  aster_app_mgr_read_manifest(const char *id, aster_manifest_t *m);

/* runtime -> manager */
void aster_app_mgr_set_state(const char *id, aster_app_state_t st, uint32_t err);
void aster_app_mgr_note_start(const char *id, uint64_t now_ms);
void aster_app_mgr_note_mem(const char *id, uint32_t linear_bytes);

/* Registry-change hook: called on the caller thread right after scan/
 * install/uninstall change the record list (e.g. the UI's resident
 * system-font builder flags itself dirty here). One slot; set NULL to
 * clear. The callback must be quick and must not re-enter the manager. */
void aster_app_mgr_set_change_cb(void (*cb)(void));
/* downgrade the AOT preference at runtime (corrupt app.aot -> wasm fallback) */
void aster_app_mgr_clear_aot(const char *id);

#endif
