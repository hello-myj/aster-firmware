#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT */
#include "aster_app_mgr.h"
#include "aster_perms.h"
#include "aster_storage.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aster_app_mgr, LOG_LEVEL_INF);

static aster_app_rec_t recs[ASTER_APP_MAX_RECS] __attribute__((section(".ext_ram.bss.app_recs")));
static int nrecs;

/* Protects recs[]/nrecs. Written by the ASTER worker thread (set_state /
 * note_start / note_mem / clear_aot, driven by the runtime) and by the
 * shell (scan / install / uninstall); read by the shell, the launcher and
 * the appmgr natives. No ISR touches the registry and no holder runs with
 * interrupts locked -- scan and uninstall do file I/O while holding it --
 * so a mutex, not a spinlock, is the right primitive.
 * Lock order: app_lock (aster_runtime.c) -> reg_lock. Nothing here reaches
 * back into the runtime while holding reg_lock, so the order is never
 * inverted. */
static K_MUTEX_DEFINE(reg_lock);

/* Registry-change hook (e.g. the UI's resident system-font builder). Runs
 * on the caller thread right after the record list changed; must be fast
 * (flag it, do the work on the UI thread) and must not re-enter. */
static void (*s_change_cb)(void);

void aster_app_mgr_set_change_cb(void (*cb)(void))
{
	k_mutex_lock(&reg_lock, K_FOREVER);
	s_change_cb = cb;
	k_mutex_unlock(&reg_lock);
}

/* recs[] lookup; the caller holds reg_lock. */
static aster_app_rec_t *find_mut(const char *id)
{
	for (int i = 0; i < nrecs; i++) {
		if (strcmp(recs[i].id, id) == 0) {
			return &recs[i];
		}
	}
	return NULL;
}

int aster_app_mgr_init(void)
{
	nrecs = 0;
	return aster_app_mgr_scan();
}

static int read_manifest_for(const char *id, char *buf, size_t n)
{
	char path[160];
	snprintf(path, sizeof(path), "%s/%s/manifest.json", ASTER_APPS_DIR, id);

	struct fs_dirent st;
	int err = fs_stat(path, &st);
	if (err || st.size >= n) {
		return -1;
	}
	struct fs_file_t f;
	fs_file_t_init(&f);
	err = fs_open(&f, path, FS_O_READ);
	if (err) {
		return -1;
	}
	err = fs_read(&f, buf, st.size);
	fs_close(&f);
	if (err < 0 || (size_t)err >= n) {
		return -1;
	}
	buf[err] = 0;
	return 0;
}

/* Record fields the runtime owns: it is the only writer of state /
 * last_error / start_count / started_at / peak_mem (set_state, note_start,
 * note_mem). A rescan rebuilds a record from what is on disk and must carry
 * these over for the app it is still running. */
struct live_snap {
	char            id[64];
	aster_app_state_t state;
	uint32_t        last_error;
	uint32_t        start_count;
	uint64_t        started_at;
	uint32_t        peak_mem;
};

static void snap_take(struct live_snap *s, const aster_app_rec_t *r)
{
	/* Exact copy, not snprintf("%s"): id is the FIRST member of
	 * aster_app_rec_t, so gcc cannot bound a string read that starts at
	 * offset 0 of recs[16] and warns about a 4224-byte one
	 * (-Wformat-truncation), which no NUL-terminated id can produce.
	 * The scan already refuses dir names >= sizeof(id) (see below), and
	 * the trailing byte forces termination regardless. */
	memcpy(s->id, r->id, sizeof(s->id));
	s->id[sizeof(s->id) - 1] = '\0';
	s->state = r->state;
	s->last_error = r->last_error;
	s->start_count = r->start_count;
	s->started_at = r->started_at;
	s->peak_mem = r->peak_mem;
}

static void snap_apply(const struct live_snap *s, aster_app_rec_t *r)
{
	r->state = s->state;
	r->last_error = s->last_error;
	r->start_count = s->start_count;
	r->started_at = s->started_at;
	r->peak_mem = s->peak_mem;
}

int aster_app_mgr_scan(void)
{
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);

	k_mutex_lock(&reg_lock, K_FOREVER);

	int err = fs_opendir(&dir, ASTER_APPS_DIR);
	if (err) {
		/* keep the previous list: a transient FS error must not make
		 * the running app vanish from the registry */
		k_mutex_unlock(&reg_lock);
		return err;
	}

	/* Rebuilding from disk must not post the app the runtime is running as
	 * STOPPED: uninstall (which would then delete a live app's directory)
	 * and start's EBUSY check both key off that state, and the runtime
	 * holds a borrowed ctx->rec across the launch. The record state is
	 * already the runtime's view of liveness (it is the only writer of
	 * RUNNING/PAUSED), so snapshot what it owns and re-apply it to the
	 * rebuilt record with the same id. V1 runs one app at a time, so at
	 * most one record can be live. */
	struct live_snap snap;
	bool have_snap = false;

	for (int i = 0; i < nrecs; i++) {
		if (recs[i].state == ASTER_APP_STATE_RUNNING ||
		    recs[i].state == ASTER_APP_STATE_PAUSED) {
			snap_take(&snap, &recs[i]);
			have_snap = true;
		}
	}

	nrecs = 0;

	struct fs_dirent ent;
	/* must match read_manifest below and install's writer, or an app that
	 * installs cleanly disappears here -- see ASTER_MANIFEST_MAX */
	char jbuf[ASTER_MANIFEST_MAX];
	while (fs_readdir(&dir, &ent) == 0 && ent.name[0] && nrecs < ASTER_APP_MAX_RECS) {
		if (ent.type != FS_DIR_ENTRY_DIR) {
			continue;
		}
		if (read_manifest_for(ent.name, jbuf, sizeof(jbuf)) != 0) {
			/* Not an app as far as we can tell -- but "unreadable
			 * manifest" and "was never an app" are different things, and
			 * the install path is the only writer. A manifest too large
			 * for this buffer, or truncated by a failed install, makes a
			 * package that is fully on disk yet never appears. Skipping
			 * in silence is what kept that invisible; the bound here and
			 * the one in aster_mja_install_file must stay consistent. */
			LOG_WRN("app dir %s: no readable manifest, skipped", ent.name);
			continue;
		}
		aster_app_rec_t *r = &recs[nrecs++];
		memset(r, 0, sizeof(*r));
		r->state = ASTER_APP_STATE_STOPPED;
		/* Explicit, because 0 is ASTER_ORIENT_PORTRAIT: the memset above
		 * would otherwise make every app in the list ask for portrait. */
		r->want_orientation = ASTER_ORIENT_ANY;
		if (strlen(ent.name) >= sizeof(r->id)) {
			nrecs--; /* id too long to track; skip */
			continue;
		}
		memcpy(r->id, ent.name, strlen(ent.name) + 1);
		aster_json_str(jbuf, "name", r->name, sizeof(r->name));
		aster_json_str(jbuf, "version", r->version, sizeof(r->version));
		aster_json_str(jbuf, "entry", r->entry, sizeof(r->entry));
		aster_json_str(jbuf, "abi_min", r->abi_min, sizeof(r->abi_min));
		r->abi_major = r->abi_min[0] ? (uint32_t)atoi(r->abi_min) : 1;
		uint32_t v;
		if (aster_json_u32(jbuf, "mem_initial", &v) == 0) r->mem_initial = v;
		if (aster_json_u32(jbuf, "mem_max", &v) == 0) r->mem_max = v;
		if (aster_json_u32(jbuf, "mem_stack", &v) == 0) r->mem_stack = v;
		if (aster_json_u32(jbuf, "mem_heap", &v) == 0) r->mem_heap = v;
		if (aster_json_u32(jbuf, "aot", &v) == 0) r->has_aot = v != 0;
		/* quote-delimited key match, so "aot" above cannot pick this up.
		 * No cast: the id is 32 bits on both sides (it was a uint8_t
		 * here, which turned target 999 into 231 by the time `app info`
		 * printed it). */
		if (aster_json_u32(jbuf, "aot_skip", &v) == 0) r->aot_skip = v;
		/* permission mask (ABI 1.2 enforcement); packed at scan so the
		 * runtime gate never touches the FS at call time */
		char perms[8][24];
		int pn = aster_json_str_array(jbuf, "permissions", perms, 8);
		r->perm_mask = aster_perm_mask(perms, (uint32_t)pn);
		if (!r->name[0]) {
			strncpy(r->name, r->id, sizeof(r->name) - 1);
		}
		/* sanity: an executable image must exist on disk. Mirror ctx_load's
		 * fallback: prefer the AOT image, but a missing app.aot is not
		 * fatal as long as app.wasm is present (interpreter path). */
		char path[192];
		int have = 0;
		if (r->has_aot && IS_ENABLED(CONFIG_ASTER_AOT)) {
			snprintf(path, sizeof(path), "%s/%s/app.aot", ASTER_APPS_DIR, r->id);
			have = fs_stat(path, &ent) == 0;
		}
		if (!have) {
			snprintf(path, sizeof(path), "%s/%s/app.wasm", ASTER_APPS_DIR, r->id);
			have = fs_stat(path, &ent) == 0;
			if (have) {
				r->has_aot = false; /* fall back to wasm image */
			}
		}
		if (!have) {
			nrecs--;
		} else {
			char cat[16];
			char orient[16];
			struct fs_dirent st;

			if (aster_json_str(jbuf, "category", cat, sizeof(cat)) >= 0) {
				aster_mja_norm_category(cat, r->category, sizeof(r->category));
			}
			if (aster_json_str(jbuf, "orientation", orient,
					 sizeof(orient)) >= 0) {
				r->want_orientation = aster_mja_norm_orientation(orient);
			}
			snprintf(path, sizeof(path), "%s/%s/icon.bin", ASTER_APPS_DIR, r->id);
			r->has_icon = fs_stat(path, &st) == 0 &&
				      st.type == FS_DIR_ENTRY_FILE && st.size >= 16;
		}
	}
	fs_closedir(&dir);

	if (have_snap) {
		aster_app_rec_t *r = find_mut(snap.id);
		if (r) {
			snap_apply(&snap, r);
		}
	}

	int n = nrecs;
	void (*cb)(void) = s_change_cb;
	k_mutex_unlock(&reg_lock);

	/* fire the hook unlocked: it must not re-enter (see the header) and a
	 * lock held here would make any such call a self-deadlock */
	if (cb) {
		cb();
	}
	return n;
}

int aster_app_mgr_count(void)
{
	k_mutex_lock(&reg_lock, K_FOREVER);
	int n = nrecs;
	k_mutex_unlock(&reg_lock);
	return n;
}

const aster_app_rec_t *aster_app_mgr_get(int idx)
{
	k_mutex_lock(&reg_lock, K_FOREVER);
	const aster_app_rec_t *r = (idx < 0 || idx >= nrecs) ? NULL : &recs[idx];
	k_mutex_unlock(&reg_lock);
	return r;
}

/* Borrowed pointer (see aster_app_mgr_find in the header): the lock only
 * makes the lookup atomic -- the record itself is neither refcounted nor
 * pinned, and the next scan/install/uninstall may move or reuse this slot.
 * Callers must copy what they need before returning to a long-lived
 * reference, or hold no record across a registry change. */
const aster_app_rec_t *aster_app_mgr_find(const char *id)
{
	k_mutex_lock(&reg_lock, K_FOREVER);
	const aster_app_rec_t *r = find_mut(id);
	k_mutex_unlock(&reg_lock);
	return r;
}

int aster_app_mgr_install(const char *src_path)
{
	int err = aster_mja_install_file(src_path);
	if (err == ASTER_OK) {
		aster_app_mgr_scan();
	}
	return err;
}

static int rmrf(const char *path)
{
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	int err = fs_opendir(&dir, path);
	if (err) {
		return err;
	}
	struct fs_dirent ent;
	while (fs_readdir(&dir, &ent) == 0 && ent.name[0]) {
		char child[256];
		size_t plen = strlen(path);
		size_t nlen = strlen(ent.name);

		if (plen + 1 + nlen >= sizeof(child)) {
			continue; /* unremovably deep path; leave it on disk */
		}
		memcpy(child, path, plen);
		child[plen] = '/';
		memcpy(child + plen + 1, ent.name, nlen + 1);
		if (ent.type == FS_DIR_ENTRY_DIR) {
			rmrf(child);
		} else {
			fs_unlink(child);
		}
	}
	fs_closedir(&dir);
	return fs_unlink(path);
}

int aster_app_mgr_uninstall(const char *id)
{
	k_mutex_lock(&reg_lock, K_FOREVER);

	aster_app_rec_t *r = find_mut(id);
	if (!r) {
		k_mutex_unlock(&reg_lock);
		return ASTER_ENOENT;
	}
	/* RUNNING/PAUSED is the runtime's live state, and it marks the record
	 * before it takes the ctx over (aster_app_start), so a record that is
	 * STOPPED here is not merely *looking* idle while the runtime is
	 * loading or tearing it down: nothing is using this app's directory. */
	if (r->state != ASTER_APP_STATE_STOPPED && r->state != ASTER_APP_STATE_CRASHED) {
		k_mutex_unlock(&reg_lock);
		return ASTER_EBUSY;
	}
	/* Unpublish before touching the FS: aster_app_start -> aster_app_mgr_find
	 * runs under this lock, so once the record is gone it can no longer
	 * pick the app up and start loading from a directory that is being
	 * deleted underneath it. */
	for (int i = 0; i < nrecs; i++) {
		if (strcmp(recs[i].id, id) == 0) {
			for (int j = i; j < nrecs - 1; j++) {
				recs[j] = recs[j + 1];
			}
			nrecs--;
			break;
		}
	}
	void (*cb)(void) = s_change_cb;
	k_mutex_unlock(&reg_lock);

	char dir[160];
	aster_storage_app_dir(dir, sizeof(dir), id);
	int err = rmrf(dir);
	if (err && err != -ENOENT) {
		LOG_ERR("uninstall %s: %d", id, err);
		/* the directory survived (possibly half-deleted): rebuild the
		 * registry from what is actually on disk -- scan() fires the
		 * change hook itself */
		aster_app_mgr_scan();
		return ASTER_EIO;
	}
	LOG_INF("uninstalled %s", id);
	if (cb) {
		cb();
	}
	return ASTER_OK;
}

int aster_app_mgr_read_manifest(const char *id, aster_manifest_t *m)
{
	/* was 512: anything the writer produced between 512 and 831 bytes read
	 * back as ASTER_ENOENT for an app the scan had just listed successfully */
	char jbuf[ASTER_MANIFEST_MAX];

	if (read_manifest_for(id, jbuf, sizeof(jbuf)) != 0) {
		return ASTER_ENOENT;
	}
	memset(m, 0, sizeof(*m));
	/* not covered by the memset -- see ASTER_ORIENT_ANY */
	m->orientation = ASTER_ORIENT_ANY;
	strncpy(m->id, id, sizeof(m->id) - 1);
	aster_json_str(jbuf, "name", m->name, sizeof(m->name));
	aster_json_str(jbuf, "version", m->version, sizeof(m->version));
	/* normalized on-disk manifest has flattened abi_min/abi_target */
	aster_json_str(jbuf, "abi_min", m->abi_min, sizeof(m->abi_min));
	aster_json_str(jbuf, "abi_target", m->abi_target, sizeof(m->abi_target));
	uint32_t v;
	if (aster_json_u32(jbuf, "mem_initial", &v) == 0) m->mem_initial = v;
	if (aster_json_u32(jbuf, "mem_max", &v) == 0)     m->mem_max = v;
	if (aster_json_u32(jbuf, "mem_stack", &v) == 0)   m->mem_stack = v;
	if (aster_json_u32(jbuf, "mem_heap", &v) == 0)    m->mem_heap = v;
	m->requires_n = aster_json_str_array(jbuf, "requires", m->requires, 8);
	m->optional_n = aster_json_str_array(jbuf, "optional", m->optional, 8);
	m->permissions_n = aster_json_str_array(jbuf, "permissions", m->permissions, 8);
	{
		char orient[16];

		if (aster_json_str(jbuf, "orientation", orient,
				 sizeof(orient)) >= 0) {
			m->orientation = aster_mja_norm_orientation(orient);
		}
	}
	return ASTER_OK;
}

void aster_app_mgr_clear_aot(const char *id)
{
	k_mutex_lock(&reg_lock, K_FOREVER);
	aster_app_rec_t *r = find_mut(id);
	if (r) {
		r->has_aot = false;
	}
	k_mutex_unlock(&reg_lock);
}

void aster_app_mgr_set_state(const char *id, aster_app_state_t st, uint32_t err)
{
	k_mutex_lock(&reg_lock, K_FOREVER);
	aster_app_rec_t *r = find_mut(id);
	if (!r) {
		k_mutex_unlock(&reg_lock);
		return;
	}
	r->state = st;
	r->last_error = err;
	if (st == ASTER_APP_STATE_STOPPED || st == ASTER_APP_STATE_CRASHED) {
		r->peak_mem = 0;
	}
	k_mutex_unlock(&reg_lock);
}

void aster_app_mgr_note_start(const char *id, uint64_t now_ms)
{
	k_mutex_lock(&reg_lock, K_FOREVER);
	aster_app_rec_t *r = find_mut(id);
	if (!r) {
		k_mutex_unlock(&reg_lock);
		return;
	}
	r->start_count++;
	r->started_at = now_ms;
	k_mutex_unlock(&reg_lock);
}

/* called once per frame by the running app: keep the critical section
 * (and the lock wait it can impose on the shell's scan/install) minimal */
void aster_app_mgr_note_mem(const char *id, uint32_t linear_bytes)
{
	k_mutex_lock(&reg_lock, K_FOREVER);
	aster_app_rec_t *r = find_mut(id);
	if (!r) {
		k_mutex_unlock(&reg_lock);
		return;
	}
	if (linear_bytes > r->peak_mem) {
		r->peak_mem = linear_bytes;
	}
	k_mutex_unlock(&reg_lock);
}
