#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * `aster` shell: system info, app install/list/start/stop/..., log.
 * Primary control plane for ASTER V1 (no system UI).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/sys_heap.h>
#if defined(CONFIG_COMMON_LIBC_MALLOC) && defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
#include <sys_malloc.h>
#endif
#include <aster/aster.h>
#include "aster_app_mgr.h"
#include "aster_mem.h"
#include "aster_runtime.h"
#include "aster_sysinfo.h"
#include "aster_storage.h"
#include "aster_logring.h"
#if IS_ENABLED(CONFIG_ASTER_SCREENSHOT)
#include "ui_screenshot.h"
#if IS_ENABLED(CONFIG_ASTER_LAUNCHER_NATIVE)
#include "launcher/launcher.h"
#endif
#endif
#if IS_ENABLED(CONFIG_ASTER_LVGL)
#include <lvgl.h>
#include "font_sd.h"
#endif

LOG_MODULE_REGISTER(aster_cmd, LOG_LEVEL_INF);

static const char *state_str(aster_app_state_t s)
{
	switch (s) {
	case ASTER_APP_STATE_RUNNING: return "RUNNING";
	case ASTER_APP_STATE_PAUSED:  return "PAUSED";
	case ASTER_APP_STATE_CRASHED: return "CRASHED";
	default:                    return "STOPPED";
	}
}

/* aster_error_t -> human hint, so `failed: -6` is actionable without the
 * error.h cheat sheet (design doc: negative codes are stable ABI). */
static const char *aster_err_str(int err)
{
	switch (err) {
	case ASTER_EINVAL:    return "invalid arg";
	case ASTER_ENOMEM:    return "out of memory";
	case ASTER_ENOTSUP:   return "not supported on this build";
	case ASTER_EPERM:     return "permission denied";
	case ASTER_EBUSY:     return "busy, stop the running app first";
	case ASTER_ENOENT:    return "no such app id (see `aster app list`)";
	case ASTER_EIO:       return "SD I/O error (reseat card?)";
	case ASTER_ETIMEDOUT: return "app wedged, timeout";
	case ASTER_EEXIST:    return "already exists";
	case ASTER_EBADF:     return "bad handle";
	case ASTER_ENOSPC:    return "out of space";
	case ASTER_ESTATE:    return "lifecycle state conflict (double pause/resume? init refused?)";
	case ASTER_EABI:      return "ABI incompatible";
	case ASTER_ECORRUPT:  return "corrupt package";
	case ASTER_EAGAIN:    return "try again";
	case ASTER_ENODEV:    return "no such device";
	case ASTER_ESIGN:     return "bad signature (wrong key / tampered)";
	case ASTER_EUNSIGNED: return "unsigned package (policy requires a signature)";
	default:            return "error";
	}
}

/* The two things that make the UI look wrong, and that no other command can
 * answer: the LVGL heap (a separate 512 KB pool -- LV_MEM_SIZE in lv_conf.h --
 * so it is invisible to `aster sys mem`) and whether the one baked cpfont face
 * actually loaded.
 *
 * No object-tree walk on purpose. LVGL is built with LV_USE_OS = LV_OS_NONE,
 * so there is no lock to take and the shell thread racing the UI thread over
 * the object list is a real (if unlikely) fault -- lv_mem_monitor() only reads
 * the allocator, which is worth the momentarily-stale sample. `peak` is the
 * high-water mark and is the number a pool resize has to be sized against;
 * `largest free block` decides whether a full-screen allocation like
 * lv_snapshot_take() can still succeed.
 *
 * The face line exists because the single-face bake turned every font failure
 * into the same picture: the placeholder font draws one filled box per glyph
 * (ui_font.c), so a card without ui_20.cpfont shows a page of green boxes with
 * no error anywhere the user can see. This is where that question gets
 * answered. ui_font.c logs it once too, but only to the console at boot. */
#if IS_ENABLED(CONFIG_ASTER_LVGL)
static int cmd_sys_lvgl(const struct shell *sh, size_t argc, char **argv)
{
	lv_mem_monitor_t m;
	uint8_t px[ASTER_FONT_BAKED_N] = ASTER_FONT_BAKED_PX;

	ARG_UNUSED(argc); ARG_UNUSED(argv);
	lv_mem_monitor(&m);
	shell_print(sh, "lvgl pool:  size=%zu used=%zu (%u%%) peak=%zu free=%zu",
		    m.total_size, m.total_size - m.free_size,
		    (unsigned)m.used_pct, m.max_used, m.free_size);
	shell_print(sh, "            largest free block=%zu B  frag=%u%%  "
		    "blocks=%zu in use", m.free_biggest_size,
		    (unsigned)m.frag_pct, m.used_cnt);
	for (uint32_t k = 0; k < ARRAY_SIZE(px); k++) {
		const lv_font_t *f = aster_font_sd_get(px[k]);

		if (f) {
			shell_print(sh, "font %2u px: loaded (line_h=%d, "
				    "base_line=%d)", (unsigned)px[k],
				    (int)f->line_height, (int)f->base_line);
		} else {
			shell_print(sh, "font %2u px: MISSING -- labels draw "
				    "placeholder boxes; /SD:/aster/fonts/ui_%u.cpfont "
				    "unreadable or still streaming",
				    (unsigned)px[k], (unsigned)px[k]);
		}
	}
	if (aster_font_sd_streaming()) {
		shell_print(sh, "            (a metric table is still "
			    "streaming: %u%%)", (unsigned)aster_font_sd_progress());
	}
	return 0;
}
#endif

static int cmd_sys_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	aster_system_info_t i;
	aster_sysinfo_get(&i);
	shell_print(sh, "ASTER %s", ASTER_SYSVER);
	shell_print(sh, "platform=%s board=%s soc=%s arch=%s abi=%u.%u.%u",
		    i.platform, i.board, i.soc, aster_arch_name(i.cpu_arch),
		    ASTER_ABI_MAJOR, ASTER_ABI_MINOR, ASTER_ABI_PATCH);
	shell_print(sh, "cpu=%u hz cores=%u", i.cpu_freq_hz, i.cpu_count);
	shell_print(sh, "ram int=%u ext=%u flash=%u", i.ram_internal, i.ram_external,
		    i.flash_size);
	shell_print(sh, "sd_mounted=%s apps=%d", aster_storage_mounted() ? "yes" : "no",
		    aster_app_mgr_count());
	const char *sigpol = "off";
	if (IS_ENABLED(CONFIG_ASTER_SIGNATURE_REQUIRE)) {
		sigpol = "require";
	} else if (IS_ENABLED(CONFIG_ASTER_SIGNATURE_ALLOW_UNSIGNED)) {
		sigpol = "allow-unsigned";
	}
	shell_print(sh, "sigpolicy=%s", sigpol);
	return 0;
}

static int cmd_sys_features(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	aster_system_info_t i;
	aster_sysinfo_get(&i);
	static const char *names[] = {
		"display", "touch", "mouse", "keyboard", "camera", "audio-in",
		"audio-out", "wifi", "bluetooth", "accelerometer", "gyroscope",
		"filesystem", "rtc", "psram", "framebuffer" };
	shell_print(sh, "features:");
	for (size_t b = 0; b < ARRAY_SIZE(names); b++) {
		if (i.feature_flags & (1ull << b)) {
			shell_print(sh, "  %s", names[b]);
		}
	}
	return 0;
}

static int cmd_sys_abi(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	shell_print(sh, "ASTER ABI %u.%u.%u", ASTER_ABI_MAJOR, ASTER_ABI_MINOR, ASTER_ABI_PATCH);
	return 0;
}

/* Internal DRAM is the scarce resource on a PSRAM-equipped board: AOT
 * executable text, DMA-reachable buffers and every thread stack must live in
 * it. Only the heap part moves at runtime, so that is what this reports --
 * the static part is a build-time number (`west build` prints dram0_0_seg)
 * and stacks have their own view in `kernel thread stacks`. Sized against
 * `peak`, not `used`: the peak is what a cut would have to survive. */
extern struct k_heap _system_heap;

#if IS_ENABLED(CONFIG_ASTER_LVGL)
static int cmd_sys_fonts(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argc);ARG_UNUSED(argv);
 unsigned total=0;
 for(uint32_t i=0;i<aster_font_sd_face_n();i++) {
  aster_font_cache_status_t st;
  if(!aster_font_sd_cache_status(i,&st)) continue;
  shell_print(sh,"%upx: %s coverage=%u cached=%u bitmap=%u alloc=%u B",
    st.px,st.available?"SD":"missing",st.coverage,st.glyphs,st.used,st.bytes);
  total+=st.bytes;
 }
 shell_print(sh,"glyph cache total=%u B (interval metadata separate)",total);
 aster_font_sd_stats_t counters;
 aster_font_sd_stats(&counters);
 shell_print(sh,"glyph stats hits=%u misses=%u filled=%u evictions=%u reads=%u bytes=%u",
   counters.hits,counters.misses,counters.miss_filled,counters.evictions,counters.reads,counters.read_bytes);
 shell_print(sh,"glyph time load_us=%u max_load_us=%u warm_us=%u warm_calls=%u (synchronous, no queue)",
   counters.load_us,counters.max_load_us,counters.warm_us,counters.warm_bursts);
 shell_print(sh,"glyph IO open_us=%u record_us=%u bitmap_us=%u close_us=%u failures=%u (close includes scope cleanup)",
   counters.open_us,counters.record_us,counters.bitmap_us,counters.close_us,counters.load_failures);

 return 0;
}
#endif

static int cmd_sys_mem(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	struct sys_memory_stats st = { 0 };

	if (sys_heap_runtime_stats_get(&_system_heap.heap, &st) == 0) {
		shell_print(sh, "dram heap:  size=%u peak=%u used=%u free=%u",
			    (unsigned)CONFIG_HEAP_MEM_POOL_SIZE,
			    (unsigned)st.max_allocated_bytes,
			    (unsigned)st.allocated_bytes,
			    (unsigned)st.free_bytes);
		shell_print(sh, "            (kernel allocations; libc worker stacks reported separately)");
	} else {
		shell_print(sh, "dram heap:  size=%u (no runtime stats)",
			    (unsigned)CONFIG_HEAP_MEM_POOL_SIZE);
	}
#if defined(CONFIG_COMMON_LIBC_MALLOC) && defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	/* This pool shares the SRAM tail with the kernel heap. Report its own
	 * statistics: _heap_sentry - _end includes the kernel reservation and
	 * must not be mistaken for the libc arena. Size excludes heap metadata. */
	struct sys_memory_stats libc_st = { 0 };
	if (malloc_runtime_stats_get(&libc_st) == 0) {
		shell_print(sh, "libc heap:  usable=%u peak=%u used=%u free=%u",
			    (unsigned)(libc_st.allocated_bytes + libc_st.free_bytes),
			    (unsigned)libc_st.max_allocated_bytes,
			    (unsigned)libc_st.allocated_bytes,
			    (unsigned)libc_st.free_bytes);
		shell_print(sh, "            (malloc/calloc; APP, Wi-Fi, audio and service worker stacks)");
	}
#endif
	aster_system_info_t i;

	aster_sysinfo_get(&i);
	shell_print(sh, "ext ram:    %u total, wamr_cap=%u", i.ram_external,
		    (unsigned)CONFIG_ASTER_WAMR_HEAP_SIZE);
	{
		/* The declared size above is a Kconfig number; this is what the
		 * image was actually linked against. Both are printed so a port
		 * whose declaration drifted from its linker script is visible
		 * here instead of showing up later as unexplained ENOMEM. The
		 * figures match docs/memory.md's PSRAM block by construction --
		 * mem_ledger.py reads the same three symbols. */
		aster_psram_geom_t g;

		if (aster_psram_geom_get(&g)) {
			shell_print(sh, "psram:      static=%zu heap=%zu used=%zu of %zu (%u%%)",
				    g.static_sz, g.heap, g.total, g.chip,
				    (unsigned)(g.chip ? 100u * g.total / g.chip : 0u));
			if (g.chip && g.total > g.chip) {
				shell_print(sh, "            MISMATCH: linked %zu B over the "
					    "declared %zu B part", g.total, g.chip);
			}
		} else {
			shell_print(sh, "psram:      no linker geometry on this port");
		}
	}
	{
		/* Probe the shared external heap the way a driver would: ask
		 * for a block, hand it straight back. The external heap has no
		 * runtime stats API, and "how big a block can still be
		 * satisfied" is exactly the number that decides whether the
		 * camera / decoder / a second full-screen buffer will come up
		 * or fail with -ENOMEM. */
		static const size_t probe[] = {
			7864320u, 7340032u, 6291456u, 5242880u, 4194304u,
			2097152u, 1048576u, 524288u, 393216u,
			307200u, 262144u, 204800u, 153600u, 131072u, 65536u,
		};
		size_t best = 0;

		for (size_t k = 0; k < ARRAY_SIZE(probe); k++) {
			void *p = shared_multi_heap_aligned_alloc(
				SMH_REG_ATTR_EXTERNAL, 32, probe[k]);

			if (p) {
				shared_multi_heap_free(p);
				best = probe[k];
				break;
			}
		}
		shell_print(sh, "ext heap:   largest free block >= %zu B", best);

		/* Hold as many full-frame blocks at once as the heap allows:
		 * that is exactly what the camera (3 x 150 KB) and the flip
		 * double buffer (2 x 150 KB) compete for. */
		{
			enum { HOLD_MAX = 8 };
			void *hold[HOLD_MAX];
			int n = 0;

			while (n < HOLD_MAX) {
				void *p = shared_multi_heap_aligned_alloc(
					SMH_REG_ATTR_EXTERNAL, 32, 153600u);

				if (!p) {
					break;
				}
				hold[n++] = p;
			}
			for (int k = 0; k < n; k++) {
				shared_multi_heap_free(hold[k]);
			}
			shell_print(sh, "ext heap:   %d x 150 KB blocks fit at once", n);
		}
	}
	shell_print(sh, "stacks:     see `kernel thread stacks`");
	shell_print(sh, "static dram: build-time; `west build` prints dram0_0_seg");
	return 0;
}

static int cmd_app_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	int n = aster_app_mgr_count();
	if (n == 0) {
		shell_print(sh, "no apps installed");
		return 0;
	}
	shell_print(sh, "%-28s %-16s %-8s %s", "ID", "NAME", "VER", "STATE");
	for (int i = 0; i < n; i++) {
		const aster_app_rec_t *r = aster_app_mgr_get(i);
		/* "[AOT!]" is not decoration: it means the package shipped an
		 * AOT image for another CPU and install dropped it, so this app
		 * runs interpreted and much slower. `aster app info <id>` says
		 * which target it wanted. */
		shell_print(sh, "%-28s %-16s %-8s %s%s", r->id, r->name, r->version,
			    state_str(r->state),
			    r->has_aot ? " [AOT]" : (r->aot_skip ? " [AOT!]" : ""));
	}
	return 0;
}

#ifdef CONFIG_ASTER_PKG_FAULT_INJECTION
static int cmd_app_fault(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "clear")) {
		aster_pkg_fault_clear(); shell_print(sh, "fault cleared"); return 0;
	}
	if (argc != 5 || (strcmp(argv[4], "error") && strcmp(argv[4], "reboot") && strcmp(argv[4], "hold"))) {
		shell_error(sh, "fault <test.p0.ID> <stage> <open|write|sync|close|verify> <error|reboot|hold>");
		return -EINVAL;
	}
	unsigned action = !strcmp(argv[4], "hold") ? 2u : !strcmp(argv[4], "reboot") ? 1u : 0u;
	int err = aster_pkg_fault_arm(argv[1], argv[2], argv[3], action);
	shell_print(sh, err ? "fault refused: %d" : "fault armed: %d", err);
	return 0;
}
#endif

static int cmd_app_install(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: aster app install <path.ast>");
		return -EINVAL;
	}
	int err = aster_app_mgr_install(argv[1]);
	if (err == ASTER_OK) {
		shell_print(sh, "installed OK");
	} else {
		shell_error(sh, "install failed: %d (%s)", err, aster_err_str(err));
	}
	return 0;
}

static int cmd_app_uninstall(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: aster app uninstall <id>");
		return -EINVAL;
	}
	int err = aster_app_mgr_uninstall(argv[1]);
	if (err == ASTER_OK) {
		shell_print(sh, "uninstalled");
	} else {
		shell_error(sh, "uninstall failed: %d (%s)", err, aster_err_str(err));
	}
	return 0;
}

static int cmd_app_start(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: aster app start <id>");
		return -EINVAL;
	}
	int err = aster_app_start(argv[1]);
	if (err == ASTER_OK) {
		shell_print(sh, "started %s", argv[1]);
	} else {
		shell_error(sh, "start failed: %d (%s)", err, aster_err_str(err));
	}
	return 0;
}

/* ENOENT from stop/pause/resume means "id != the running app", not "id
 * unknown" — the generic hint would be misleading there. */
static int do_ctrl(const struct shell *sh, char **argv, size_t argc,
		   int (*fn)(const char *))
{
	if (argc < 2) {
		shell_error(sh, "usage: <id>");
		return -EINVAL;
	}
	int err = fn(argv[1]);
	if (err == ASTER_OK) {
		shell_print(sh, "ok");
	} else if (err == ASTER_ENOENT) {
		shell_print(sh, "failed: %d (not the running app)", err);
	} else {
		shell_print(sh, "failed: %d (%s)", err, aster_err_str(err));
	}
	return 0;
}

static int cmd_app_stop(const struct shell *sh, size_t argc, char **argv)
{
	return do_ctrl(sh, argv, argc, aster_app_stop);
}
static int cmd_app_pause(const struct shell *sh, size_t argc, char **argv)
{
	return do_ctrl(sh, argv, argc, aster_app_pause);
}
static int cmd_app_resume(const struct shell *sh, size_t argc, char **argv)
{
	return do_ctrl(sh, argv, argc, aster_app_resume);
}

static int cmd_app_state(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: aster app state <id>");
		return -EINVAL;
	}
	const aster_app_rec_t *r = aster_app_mgr_find(argv[1]);
	if (!r) {
		shell_error(sh, "no such app");
		return -EINVAL;
	}
	shell_print(sh, "%s: %s (err=%u starts=%u)", r->id, state_str(r->state),
		    r->last_error, r->start_count);
	return 0;
}

static int cmd_app_stats(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: aster app stats <id>");
		return -EINVAL;
	}
	const aster_app_rec_t *r = aster_app_mgr_find(argv[1]);
	if (!r) {
		shell_error(sh, "no such app");
		return -EINVAL;
	}
	shell_print(sh, "id=%s state=%s", r->id, state_str(r->state));
	shell_print(sh, "mem: initial=%u max=%u stack=%u heap=%u peak=%u",
		    r->mem_initial, r->mem_max, r->mem_stack, r->mem_heap, r->peak_mem);
	aster_app_ctx_t *c = aster_app_current();
	if (c && strcmp(c->id, r->id) == 0) {
		shell_print(sh, "frames=%u fps=%u uptime=%lldms", c->frame_no, c->framerate,
			    (long long)(k_uptime_get() - (int64_t)c->start_ms));
	}
	return 0;
}

static int cmd_app_info(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: aster app info <id>");
		return -EINVAL;
	}
	const aster_app_rec_t *r = aster_app_mgr_find(argv[1]);
	if (!r) {
		shell_error(sh, "no such app");
		return -EINVAL;
	}
	shell_print(sh, "id:       %s", r->id);
	shell_print(sh, "name:     %s", r->name);
	shell_print(sh, "version:  %s", r->version);
	shell_print(sh, "state:    %s", state_str(r->state));
	shell_print(sh, "entry:    %s", r->entry);
	shell_print(sh, "mem:      initial=%u max=%u stack=%u heap=%u",
		    r->mem_initial, r->mem_max, r->mem_stack, r->mem_heap);
	shell_print(sh, "image:    %s", r->has_aot ? "wasm + aot" : "wasm");
	if (r->aot_skip) {
		/* Name both ids, not just "mismatch": the fix is to rebuild the
		 * package for this device, and that needs the wanted id. */
		shell_print(sh, "aot:      DROPPED -- package is target %u, this "
			    "device runs target %u; app is interpreted",
			    r->aot_skip, (unsigned)CONFIG_ASTER_AOT_TARGET_ID);
	}
	shell_print(sh, "starts:   %u  last_err=%u  peak=%u", r->start_count,
		    r->last_error, r->peak_mem);
	/* shell thread is single-user; static avoids ~900B on its 8K stack */
	static aster_manifest_t m;
	if (aster_app_mgr_read_manifest(r->id, &m) == ASTER_OK) {
		shell_print(sh, "abi:      min=%s target=%s", m.abi_min, m.abi_target);
		if (m.requires_n) {
			shell_print(sh, "requires: %s", m.requires[0]);
			for (uint32_t i = 1; i < m.requires_n; i++) {
				shell_print(sh, "          %s", m.requires[i]);
			}
		}
		if (m.permissions_n) {
			shell_print(sh, "perms:    %s", m.permissions[0]);
			for (uint32_t i = 1; i < m.permissions_n; i++) {
				shell_print(sh, "          %s", m.permissions[i]);
			}
		}
		/* Printed from the record, not from the manifest above: that is
		 * what the runtime will actually act on, and the two are read
		 * through different parsers (manifest JSON here, the flattened
		 * on-disk line in the scan). */
		const char *on = aster_pkg_orientation_name(r->want_orientation);
		shell_print(sh, "orient:   %s", on ? on : "any (follows panel)");
	}
	return 0;
}

static int cmd_app_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	int n = aster_app_mgr_scan();
	shell_print(sh, "scan: %d apps", n);
	return 0;
}

/* log dump callback */
struct log_ctx {
	const struct shell *sh;
	const char *grep; /* NULL = all */
	int shown;
};

static bool log_print_cb(uint64_t ts, const char *app, uint32_t level, const char *msg, void *u)
{
	struct log_ctx *lc = u;
	static const char *lvl[] = { "dbg", "inf", "wrn", "err", "ftl" };
	if (lc->grep && strcmp(lc->grep, app) != 0) {
		return true;
	}
	shell_print(lc->sh, "[%8llu.%03llu] %-5s %-16s %s",
		    (unsigned long long)(ts / 1000), (unsigned long long)(ts % 1000),
		    lvl[level > 4 ? 4 : level], app, msg);
	lc->shown++;
	return true;
}

static int cmd_log(const struct shell *sh, size_t argc, char **argv)
{
	struct log_ctx lc = { .sh = sh, .grep = NULL };
	int limit = 20;

	if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
		aster_logring_clear();
		shell_print(sh, "cleared");
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "grep") == 0) {
		if (argc < 3) {
			shell_error(sh, "usage: aster log grep <id> [limit]");
			return -EINVAL;
		}
		lc.grep = argv[2];
		limit = 200;
		if (argc >= 4) {
			limit = atoi(argv[3]);
			if (limit <= 0 || limit > 500) {
				limit = 500;
			}
		}
	} else if (argc >= 2) {
		limit = atoi(argv[1]);
		if (limit <= 0 || limit > 500) {
			limit = 500;
		}
	}
	aster_logring_dump(log_print_cb, &lc, limit);
	if (lc.grep && lc.shown == 0) {
		shell_print(sh, "(no logs for %s)", lc.grep);
	}
	return 0;
}

/* ---- aster fs : unique SD file-management entry (no cd/pwd) ---- */

#define ASTER_FS_PATH_MAX 192
#define ASTER_FS_CAT_MAX  4096
#define ASTER_FS_HEX_MAX  1024

static int fs_need_sd(const struct shell *sh)
{
	if (!aster_storage_mounted()) {
		shell_error(sh, "SD not mounted");
		return -ENODEV;
	}
	return 0;
}

static int fs_resolve(const char *in, char *out, size_t n)
{
	const char *src = (in && in[0]) ? in : ASTER_MOUNT_POINT;
	int w;
	size_t len;

	if (strstr(src, "..")) {
		return -EINVAL;
	}
	if (src[0] != '/') {
		w = snprintf(out, n, "%s/%s", ASTER_MOUNT_POINT, src);
	} else if (strncmp(src, ASTER_MOUNT_POINT, 4) == 0 &&
		   (src[4] == '\0' || src[4] == '/')) {
		w = snprintf(out, n, "%s", src);
	} else {
		return -EINVAL;
	}
	if (w < 0 || (size_t)w >= n) {
		return -ENAMETOOLONG;
	}
	len = strlen(out);
	while (len > 4 && out[len - 1] == '/') {
		out[--len] = 0;
	}
	return 0;
}

static int cmd_fs_ls(const struct shell *sh, size_t argc, char **argv)
{
	char path[ASTER_FS_PATH_MAX];
	struct fs_dir_t dir;
	struct fs_dirent ent;

	if (fs_need_sd(sh)) {
		return 0;
	}
	if (fs_resolve(argc >= 2 ? argv[1] : NULL, path, sizeof(path))) {
		shell_error(sh, "bad path");
		return 0;
	}
	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, path)) {
		shell_error(sh, "open %s failed", path);
		return 0;
	}
	unsigned skipped = 0;

	while (fs_readdir(&dir, &ent) == 0 && ent.name[0]) {
		if (!aster_fs_name_valid(ent.name)) {
			skipped++;
			continue;
		}
		shell_print(sh, "%c %8llu  %s",
			    ent.type == FS_DIR_ENTRY_DIR ? 'd' : '-',
			    ent.type == FS_DIR_ENTRY_DIR ? 0ULL :
				    (unsigned long long)ent.size,
			    ent.name);
	}
	fs_closedir(&dir);
	if (skipped) {
		shell_warn(sh, "%u invalid entries skipped", skipped);
	}
	return 0;
}

static int cmd_fs_stat(const struct shell *sh, size_t argc, char **argv)
{
	char path[ASTER_FS_PATH_MAX];
	struct fs_dirent st;

	if (fs_need_sd(sh)) {
		return 0;
	}
	if (argc < 2 || fs_resolve(argv[1], path, sizeof(path))) {
		shell_error(sh, "usage: aster fs stat <path>");
		return 0;
	}
	if (fs_stat(path, &st)) {
		shell_error(sh, "stat %s failed", path);
		return 0;
	}
	shell_print(sh, "%s %s %llu",
		    st.type == FS_DIR_ENTRY_DIR ? "dir" : "file", path,
		    (unsigned long long)st.size);
	return 0;
}

static int cmd_fs_cat(const struct shell *sh, size_t argc, char **argv)
{
	char path[ASTER_FS_PATH_MAX];
	struct fs_file_t f;
	char buf[128];
	size_t total = 0;
	int r;

	if (fs_need_sd(sh)) {
		return 0;
	}
	if (argc < 2 || fs_resolve(argv[1], path, sizeof(path))) {
		shell_error(sh, "usage: aster fs cat <path>");
		return 0;
	}
	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ)) {
		shell_error(sh, "open %s failed", path);
		return 0;
	}
	while (total < ASTER_FS_CAT_MAX) {
		size_t chunk = sizeof(buf);

		if (chunk > ASTER_FS_CAT_MAX - total) {
			chunk = ASTER_FS_CAT_MAX - total;
		}
		r = fs_read(&f, buf, chunk);
		if (r <= 0) {
			break;
		}
		for (int i = 0; i < r; i++) {
			unsigned char c = (unsigned char)buf[i];

			if (c == '\n' || c == '\r' || c == '\t' ||
			    (c >= 32 && c < 127)) {
				shell_fprintf(sh, SHELL_NORMAL, "%c", c);
			} else {
				shell_fprintf(sh, SHELL_NORMAL, ".");
			}
		}
		total += (size_t)r;
	}
	fs_close(&f);
	if (total == 0) {
		shell_print(sh, "(empty)");
	} else {
		shell_print(sh, "");
		if (total >= ASTER_FS_CAT_MAX) {
			shell_print(sh, "... truncated");
		}
	}
	return 0;
}

static int cmd_fs_hex(const struct shell *sh, size_t argc, char **argv)
{
	char path[ASTER_FS_PATH_MAX];
	struct fs_file_t f;
	uint8_t buf[16];
	size_t total = 0;
	int r;

	if (fs_need_sd(sh)) {
		return 0;
	}
	if (argc < 2 || fs_resolve(argv[1], path, sizeof(path))) {
		shell_error(sh, "usage: aster fs hex <path>");
		return 0;
	}
	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ)) {
		shell_error(sh, "open %s failed", path);
		return 0;
	}
	while (total < ASTER_FS_HEX_MAX) {
		r = fs_read(&f, buf, sizeof(buf));
		if (r <= 0) {
			break;
		}
		shell_fprintf(sh, SHELL_NORMAL, "%04x:", (unsigned)total);
		for (int i = 0; i < r; i++) {
			shell_fprintf(sh, SHELL_NORMAL, " %02x", buf[i]);
		}
		shell_print(sh, "");
		total += (size_t)r;
		if ((size_t)r < sizeof(buf)) {
			break;
		}
	}
	fs_close(&f);
	if (total == 0) {
		shell_print(sh, "(empty)");
	} else if (total >= ASTER_FS_HEX_MAX) {
		shell_print(sh, "... truncated");
	}
	return 0;
}

static int cmd_fs_mkdir(const struct shell *sh, size_t argc, char **argv)
{
	char path[ASTER_FS_PATH_MAX];
	int err;

	if (fs_need_sd(sh)) {
		return 0;
	}
	if (argc < 2 || fs_resolve(argv[1], path, sizeof(path))) {
		shell_error(sh, "usage: aster fs mkdir <path>");
		return 0;
	}
	err = aster_storage_mkdir_p(path);
	shell_print(sh, err ? "mkdir failed %d" : "ok", err);
	return 0;
}

static int cmd_fs_rm(const struct shell *sh, size_t argc, char **argv)
{
	char path[ASTER_FS_PATH_MAX];
	int err;

	if (fs_need_sd(sh)) {
		return 0;
	}
	if (argc < 2 || fs_resolve(argv[1], path, sizeof(path))) {
		shell_error(sh, "usage: aster fs rm <path>");
		return 0;
	}
	err = fs_unlink(path);
	if (err) {
		shell_error(sh, "rm failed %d (non-empty dir?)", err);
	} else {
		shell_print(sh, "removed");
	}
	return 0;
}

static int cmd_fs_cp(const struct shell *sh, size_t argc, char **argv)
{
	char src[ASTER_FS_PATH_MAX], dst[ASTER_FS_PATH_MAX];
	struct fs_file_t in, out;
	uint8_t buf[512];
	int r, w, err = 0;

	if (fs_need_sd(sh)) {
		return 0;
	}
	if (argc < 3 || fs_resolve(argv[1], src, sizeof(src)) ||
	    fs_resolve(argv[2], dst, sizeof(dst))) {
		shell_error(sh, "usage: aster fs cp <src> <dst>");
		return 0;
	}
	fs_file_t_init(&in);
	fs_file_t_init(&out);
	if (fs_open(&in, src, FS_O_READ)) {
		shell_error(sh, "open %s failed", src);
		return 0;
	}
	if (fs_open(&out, dst, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC)) {
		fs_close(&in);
		shell_error(sh, "open %s failed", dst);
		return 0;
	}
	while ((r = fs_read(&in, buf, sizeof(buf))) > 0) {
		w = fs_write(&out, buf, r);
		if (w != r) {
			err = (w < 0) ? w : -EIO;
			break;
		}
	}
	if (r < 0) {
		err = r;
	}
	fs_close(&in);
	fs_close(&out);
	shell_print(sh, err ? "cp failed %d" : "ok", err);
	return 0;
}

static int cmd_fs_mv(const struct shell *sh, size_t argc, char **argv)
{
	char src[ASTER_FS_PATH_MAX], dst[ASTER_FS_PATH_MAX];
	int err;

	if (fs_need_sd(sh)) {
		return 0;
	}
	if (argc < 3 || fs_resolve(argv[1], src, sizeof(src)) ||
	    fs_resolve(argv[2], dst, sizeof(dst))) {
		shell_error(sh, "usage: aster fs mv <src> <dst>");
		return 0;
	}
	err = fs_rename(src, dst);
	shell_print(sh, err ? "mv failed %d" : "ok", err);
	return 0;
}

static int cmd_fs_df(const struct shell *sh, size_t argc, char **argv)
{
	struct fs_statvfs st;
	uint64_t total, freeb;
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (fs_need_sd(sh)) {
		return 0;
	}
	err = fs_statvfs(ASTER_MOUNT_POINT, &st);
	if (err) {
		shell_error(sh, "not mounted (%d)", err);
		return 0;
	}
	total = (uint64_t)st.f_blocks * st.f_frsize;
	freeb = (uint64_t)st.f_bfree * st.f_frsize;
	shell_print(sh, "SD: total %llu MB, free %llu MB",
		    (unsigned long long)(total >> 20),
		    (unsigned long long)(freeb >> 20));
	return 0;
}

static int cmd_fs_mount(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	err = aster_storage_init();
	shell_print(sh, err ? "mount failed %d" : "mounted", err);
	return 0;
}

/* ---- command tree ---- */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sys,
	SHELL_CMD(info, NULL, "device profile", cmd_sys_info),
	SHELL_CMD(features, NULL, "capability list", cmd_sys_features),
	SHELL_CMD(abi, NULL, "ABI version", cmd_sys_abi),
	SHELL_CMD(mem, NULL, "memory budget (heap peak / ext ram)", cmd_sys_mem),
#if IS_ENABLED(CONFIG_ASTER_LVGL)
    SHELL_CMD(fonts, NULL, "native font sizes and glyph cache allocations", cmd_sys_fonts),
#endif
#if IS_ENABLED(CONFIG_ASTER_LVGL)
	SHELL_CMD(lvgl, NULL, "LVGL heap pool + baked font face state",
		  cmd_sys_lvgl),
#endif
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_app,
	SHELL_CMD(list, NULL, "installed apps", cmd_app_list),
#ifdef CONFIG_ASTER_PKG_FAULT_INJECTION
	SHELL_CMD(fault, NULL, "one-shot test.p0.* install fault (diagnostic build)", cmd_app_fault),
#endif
	SHELL_CMD_ARG(install, NULL, "install <path.ast>", cmd_app_install, 2, 0),
	SHELL_CMD_ARG(uninstall, NULL, "uninstall <id>", cmd_app_uninstall, 2, 0),
	SHELL_CMD_ARG(start, NULL, "start <id>", cmd_app_start, 2, 0),
	SHELL_CMD_ARG(stop, NULL, "stop <id>", cmd_app_stop, 2, 0),
	SHELL_CMD_ARG(pause, NULL, "pause <id>", cmd_app_pause, 2, 0),
	SHELL_CMD_ARG(resume, NULL, "resume <id>", cmd_app_resume, 2, 0),
	SHELL_CMD_ARG(state, NULL, "state <id>", cmd_app_state, 2, 0),
	SHELL_CMD_ARG(stats, NULL, "runtime stats <id>", cmd_app_stats, 2, 0),
	SHELL_CMD_ARG(info, NULL, "app details <id>", cmd_app_info, 2, 0),
	SHELL_CMD(scan, NULL, "rescan /SD:/aster/apps", cmd_app_scan),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_fs,
	SHELL_CMD_ARG(ls, NULL, "list [path]", cmd_fs_ls, 1, 1),
	SHELL_CMD_ARG(stat, NULL, "stat <path>", cmd_fs_stat, 2, 0),
	SHELL_CMD_ARG(cat, NULL, "text dump <path> (4K)", cmd_fs_cat, 2, 0),
	SHELL_CMD_ARG(hex, NULL, "hexdump <path> (1K)", cmd_fs_hex, 2, 0),
	SHELL_CMD_ARG(mkdir, NULL, "mkdir <path>", cmd_fs_mkdir, 2, 0),
	SHELL_CMD_ARG(rm, NULL, "remove file or empty dir", cmd_fs_rm, 2, 0),
	SHELL_CMD_ARG(cp, NULL, "copy <src> <dst>", cmd_fs_cp, 3, 0),
	SHELL_CMD_ARG(mv, NULL, "rename <src> <dst>", cmd_fs_mv, 3, 0),
	SHELL_CMD(df, NULL, "free space", cmd_fs_df),
	SHELL_CMD(mount, NULL, "mount/retry SD", cmd_fs_mount),
	SHELL_SUBCMD_SET_END
);

#if IS_ENABLED(CONFIG_ASTER_SCREENSHOT)
static int cmd_ui_screenshot(const struct shell *sh, size_t argc, char **argv)
{
	/* `ui screenshot [home|apps]` — the optional page lets an off-board
	 * debugger (sdk/tools/shell_shot.py) capture the drawer too. The
	 * launcher runs on the UI thread that services the grab, so setting
	 * the want-flag from here is safe. */
	if (argc >= 2) {
#if IS_ENABLED(CONFIG_ASTER_LAUNCHER_NATIVE)
		if (strcmp(argv[1], "home") == 0) {
			aster_launcher_show_page(0);
		} else if (strcmp(argv[1], "apps") == 0) {
			aster_launcher_show_page(1);
		} else {
			shell_error(sh, "page: home|apps");
			return -EINVAL;
		}
#else
		shell_error(sh, "page arg needs the native launcher");
		return -EINVAL;
#endif
		k_msleep(120); /* let the pump build + paint the page */
	}
	int err = aster_screenshot_request(sh);

	if (err) {
		shell_error(sh, "screenshot: %d", err);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ui,
	SHELL_CMD_ARG(screenshot, NULL, "grab screen -> zlib base64 (host: screenshot.py)",
		      cmd_ui_screenshot, 1, 1),
	SHELL_SUBCMD_SET_END
);
#endif

#if IS_ENABLED(CONFIG_ASTER_WAMR)
static int cmd_display_mode(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    enum aster_display_mode mode;
    if (!strcmp(argv[1], "headless")) mode = ASTER_DISPLAY_MODE_HEADLESS;
    else if (!strcmp(argv[1], "portrait")) mode = ASTER_DISPLAY_MODE_PORTRAIT;
    else if (!strcmp(argv[1], "landscape")) mode = ASTER_DISPLAY_MODE_LANDSCAPE;
    else { shell_error(sh, "display headless|portrait|landscape"); return -EINVAL; }
    int err = aster_runtime_display_mode_request(mode);
    if (err) shell_error(sh, "display mode refused: %d (stop the active APP first)", err);
    else shell_print(sh, "display mode queued: %s", argv[1]);
    return err;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(aster_cmds,
#if IS_ENABLED(CONFIG_ASTER_WAMR)
    SHELL_CMD_ARG(display, NULL, "headless|portrait|landscape (same firmware)", cmd_display_mode, 2, 0),
#endif
	SHELL_CMD(sys, &sub_sys, "system", NULL),
	SHELL_CMD(app, &sub_app, "apps", NULL),
	SHELL_CMD(fs, &sub_fs, "SD files", NULL),
#if IS_ENABLED(CONFIG_ASTER_SCREENSHOT)
	SHELL_CMD(ui, &sub_ui, "UI debug", NULL),
#endif
	SHELL_CMD_ARG(log, NULL, "log [limit] | grep <id> [limit] | clear", cmd_log, 1, 3),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(aster, &aster_cmds, "ASTER dynamic app platform", NULL);
