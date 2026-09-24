#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * ASTER runtime core: WAMR integration + app event loop thread.
 */
#include "aster_runtime.h"
#include "aster_native.h"
#include "aster_mem.h"
#include "aster_storage.h"
#include "aster_sysinfo.h"
#include "aster_logring.h"
#include "aster_platform.h"
#if IS_ENABLED(CONFIG_ASTER_LVGL)
#include "ui_font.h"
#include "ui_gfx.h"
#include "ui_lvgl.h"
/* The sheet is not a launcher feature: it goes up over a running app too,
 * which is why these two are included even when CONFIG_ASTER_LAUNCHER_NATIVE
 * is off (they are not under that gate in firmware/ui/CMakeLists.txt). */
#include "gesture.h"
#include "qpanel.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <aster/aster.h>
#include "wasm_export.h"
#include <zephyr/dt-bindings/input/input-event-codes.h>

LOG_MODULE_REGISTER(aster_rt, LOG_LEVEL_INF);

#define RUNTIME_POOL_SIZE CONFIG_ASTER_WAMR_HEAP_SIZE

static char *runtime_pool;
static bool runtime_inited;

/* single running app (V1) */
static aster_app_ctx_t *current;
static struct k_mutex app_lock; /* serialize start/stop vs shell */

#if IS_ENABLED(CONFIG_ASTER_LAUNCHER_NATIVE)
#include "launcher/launcher.h"
#endif

/* ABI 1.2 launch policy: deferred relaunch (handoff target / home UI).
 * Launched from the system workqueue because the app worker cannot
 * hand itself a new app (aster_app_start waits on worker sems). */
static struct k_work relaunch_work;
static char relaunch_id[64];
static void relaunch_fn(struct k_work *w);
static void relaunch_policy_after(const char *exited_id);

/* one persistent worker thread executes ALL wasm work (V1: single app).
 * WAMR exec_env is thread-bound; keeping load/init/start/event-loop/stop/
 * teardown on one thread avoids per-launch races and guarantees the
 * self-exit / trap paths always reclaim `current`. */
K_THREAD_STACK_DEFINE(app_stack_area, CONFIG_ASTER_APP_STACK_SIZE);
static struct k_thread app_thread_obj;
static aster_app_ctx_t *worker_pending; /* ctx handed to the worker */
static int worker_result;             /* start result, valid after worker_ready */
static struct k_sem worker_go;        /* given by aster_app_start */
static struct k_sem worker_ready;     /* given when start resolved (ok/err) */
static struct k_sem worker_done;      /* given when app fully torn down */

static atomic_t display_mode_request = ATOMIC_INIT(-1);

int aster_runtime_display_mode_request(enum aster_display_mode mode)
{
    if (mode < ASTER_DISPLAY_MODE_HEADLESS || mode > ASTER_DISPLAY_MODE_LANDSCAPE) {
        return ASTER_EINVAL;
    }
    if (!runtime_inited) return ASTER_ESTATE;
    k_mutex_lock(&app_lock, K_FOREVER);
    int result = ASTER_OK;
    if (current || worker_pending) result = ASTER_EBUSY;
    else atomic_set(&display_mode_request, mode);
    k_mutex_unlock(&app_lock);
    return result;
}

static void apply_display_mode(void)
{
    int mode = (int)atomic_set(&display_mode_request, -1);
    if (mode < 0) return;
#if IS_ENABLED(CONFIG_ASTER_LAUNCHER_NATIVE)
    aster_launcher_hide();
#endif
    int err = aster_plat_display_set_enabled(mode != ASTER_DISPLAY_MODE_HEADLESS);
    if (!err && mode != ASTER_DISPLAY_MODE_HEADLESS) {
        err = aster_plat_display_orientation_set(mode == ASTER_DISPLAY_MODE_PORTRAIT
                                                ? ASTER_ORIENT_PORTRAIT : ASTER_ORIENT_LANDSCAPE);
#if IS_ENABLED(CONFIG_ASTER_LVGL)
        aster_ui_lvgl_resync_window();
#endif
#if IS_ENABLED(CONFIG_ASTER_LAUNCHER_NATIVE)
        aster_launcher_request_show();
#endif
    }
    if (err) LOG_ERR("display mode %d failed: %d", mode, err);
    else LOG_INF("display mode: %s", mode == ASTER_DISPLAY_MODE_HEADLESS ? "headless" :
                 mode == ASTER_DISPLAY_MODE_PORTRAIT ? "portrait" : "landscape");
}

static void worker_fn(void *p1, void *p2, void *p3);

/* --- the orientation lease (S5 declared it, S6 honours it) ---------------
 *
 * A package may declare `orientation` in its manifest. S5 made that
 * advisory -- an unrecognised or unsatisfiable value never blocks an
 * install. S6 is the other half: for as long as such an app runs, the
 * window really is the one it asked for, and when the app goes away the
 * window comes back *before* anything else is drawn on it.
 *
 * Why "before" is the whole point: the system UI is laid out at build time
 * for the window the firmware was built for (firmware/ui's geometry
 * assertions are BUILD_ASSERTs), so it does not reflow. Restoring after
 * relaunch_policy_after() would raise the launcher on the app's window and
 * leave it there -- a sideways launcher, which is exactly the failure this
 * lease exists to prevent.
 *
 * Single global because V1 runs one app at a time (aster_app_start refuses
 * with EBUSY otherwise), and both ends run on the worker thread: it is the
 * only thread that may touch the display while LVGL is up, and taking the
 * lease there also puts the switch ahead of ctx_startup(), so the app's
 * aster_app_init sees the space it asked for on its very first query. */
static struct {
	int8_t from; /* what the window was before we changed it */
	int8_t to;   /* what we set; ASTER_ORIENT_ANY = no lease is held */
} orient_lease = { ASTER_ORIENT_ANY, ASTER_ORIENT_ANY };

static void orient_lease_take(const aster_app_rec_t *rec)
{
	const int want = rec->want_orientation;
	const char *name = aster_pkg_orientation_name((int8_t)want);
	int cur, err;

	if (!name) {
		return; /* declared nothing (ASTER_ORIENT_ANY) */
	}
	cur = aster_plat_display_orientation_get();
	if (cur < 0) {
		LOG_DBG("app %s asked for %s; this build has no display", rec->id,
			name);
		return;
	}
	if (cur == want) {
		/* Already showing it. Taking a lease here would make teardown
		 * write a register to restore a window it never changed. */
		return;
	}
	err = aster_plat_display_orientation_set(want);
	if (err) {
		/* Advisory, not a gate (S5): the package installs and runs on
		 * a board that cannot provide the window -- it just gets the
		 * one the panel shows. Loud, because an app laid out for the
		 * other aspect will look wrong and that is otherwise a
		 * mystery on the glass. */
		LOG_WRN("app %s declared %s, which this panel refused (%d); "
			"starting on %s", rec->id, name, err,
			aster_pkg_orientation_name((int8_t)cur));
		return;
	}
	orient_lease.from = (int8_t)cur;
	orient_lease.to = (int8_t)want;
	LOG_INF("app %s: window -> %s (was %s)", rec->id, name,
		aster_pkg_orientation_name((int8_t)cur));
}

/* Give the window back. Called on BOTH ways out of worker_fn -- an app that
 * failed to start holds the lease too -- and always before worker_done is
 * given, so a stop that immediately starts the next app cannot have its
 * lease cleared by this one's teardown. */
static void orient_lease_release(void)
{
	const int8_t set_to = orient_lease.to;
	const int8_t back_to = orient_lease.from;
	int cur;

	if (set_to == ASTER_ORIENT_ANY) {
		return; /* nothing was taken */
	}
	orient_lease.to = ASTER_ORIENT_ANY;
	orient_lease.from = ASTER_ORIENT_ANY;

	cur = aster_plat_display_orientation_get();
	if (cur < 0) {
		return;
	}
	if (cur != set_to) {
		/* Something turned the panel while the app ran (`hw display
		 * orient`). That is newer intent than the lease, and undoing
		 * it would look like the shell command silently failed. */
		LOG_INF("orientation: left as %s; the panel was turned while "
			"the %s lease was held", aster_pkg_orientation_name((int8_t)cur),
			aster_pkg_orientation_name(set_to));
		return;
	}
	if (aster_plat_display_orientation_set(back_to)) {
		LOG_ERR("orientation: could not restore the %s window",
			aster_pkg_orientation_name(back_to));
	} else {
		LOG_INF("orientation: window -> %s",
			aster_pkg_orientation_name(back_to));
	}
}

int aster_runtime_init(void)
{
	RuntimeInitArgs args = { 0 };

	if (runtime_inited) {
		return 0;
	}
	k_mutex_init(&app_lock);

	runtime_pool = aster_psram_alloc(RUNTIME_POOL_SIZE);
	if (!runtime_pool) {
		LOG_ERR("runtime pool alloc failed (%u)", RUNTIME_POOL_SIZE);
		return -ENOMEM;
	}

	args.mem_alloc_type = Alloc_With_Pool;
	args.mem_alloc_option.pool.heap_buf = runtime_pool;
	args.mem_alloc_option.pool.heap_size = RUNTIME_POOL_SIZE;
	args.n_native_symbols = aster_native_symbol_count();
	args.native_symbols = aster_native_symbols();
	args.native_module_name = "env";

	if (!wasm_runtime_full_init(&args)) {
		aster_psram_free(runtime_pool);
		runtime_pool = NULL;
		LOG_ERR("wasm_runtime_full_init failed");
		return -EIO;
	}
	runtime_inited = true;
	k_sem_init(&worker_go, 0, 1);
	k_sem_init(&worker_ready, 0, 1);
	k_sem_init(&worker_done, 0, 1);
	k_work_init(&relaunch_work, relaunch_fn);
	k_thread_create(&app_thread_obj, app_stack_area,
			K_THREAD_STACK_SIZEOF(app_stack_area), worker_fn,
			NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&app_thread_obj, "aster_app");

	uint32_t vmaj, vmin, vpat;
	wasm_runtime_get_version(&vmaj, &vmin, &vpat);
	LOG_INF("ASTER runtime ready (ABI %u.%u, pool %d KB, WAMR %u.%u.%u)",
		ASTER_ABI_MAJOR, ASTER_ABI_MINOR, RUNTIME_POOL_SIZE / 1024, vmaj, vmin, vpat);
	return 0;
}

aster_app_ctx_t *aster_runtime_owner(void *module_inst)
{
	aster_app_ctx_t *ctx = current;
	if (ctx && module_inst && ctx->module_inst == module_inst) {
		return ctx;
	}
	return NULL;
}

/* ---------- image loading ---------- */

static int load_image(const char *path, uint8_t **out, size_t *out_len)
{
	struct fs_dirent st;
	int err = fs_stat(path, &st);
	if (err) {
		return err;
	}
	uint8_t *b = aster_psram_alloc((size_t)st.size);
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
	if (err < 0 || (size_t)err != (size_t)st.size) {
		aster_psram_free(b);
		return err < 0 ? err : -EIO;
	}
	*out = b;
	*out_len = (size_t)err;
	return 0;
}

static int ctx_load(aster_app_ctx_t *ctx, const aster_app_rec_t *rec)
{
	char dir[192];
	char path[224];
	aster_storage_app_dir(dir, sizeof(dir), rec->id);

	const char *entry = "app.wasm";
	bool want_aot = rec->has_aot && IS_ENABLED(CONFIG_ASTER_AOT);
	if (want_aot) {
		entry = "app.aot";
	}

	snprintf(path, sizeof(path), "%s/%s", dir, entry);
	int err = load_image(path, &ctx->img, &ctx->img_size);
	if (err) {
		/* fall back to wasm if aot file is missing */
		if (want_aot) {
			snprintf(path, sizeof(path), "%s/app.wasm", dir);
			err = load_image(path, &ctx->img, &ctx->img_size);
			want_aot = false;
		}
		if (err) {
			LOG_ERR("load %s: %d", path, err);
			return err;
		}
	}

	char error_buf[160];
	ctx->module = wasm_runtime_load(ctx->img, (uint32_t)ctx->img_size,
					error_buf, sizeof(error_buf));
	if (!ctx->module && want_aot) {
		/* Corrupt/incompatible AOT image: drop it and fall back to the
		 * wasm body (same as a missing app.aot), once per boot. The
		 * bad file is removed so the next load skips straight to wasm. */
		LOG_WRN("aot load failed (%s); falling back to wasm", error_buf);
		aster_psram_free(ctx->img);
		ctx->img = NULL;
		snprintf(path, sizeof(path), "%s/app.aot", dir);
		(void)fs_unlink(path);
		aster_app_mgr_clear_aot(rec->id);
		want_aot = false;
		snprintf(path, sizeof(path), "%s/app.wasm", dir);
		err = load_image(path, &ctx->img, &ctx->img_size);
		if (err) {
			LOG_ERR("load %s: %d", path, err);
			return err;
		}
		ctx->module = wasm_runtime_load(ctx->img, (uint32_t)ctx->img_size,
						error_buf, sizeof(error_buf));
	}
	if (!ctx->module) {
		LOG_ERR("wasm_runtime_load: %s", error_buf);
		aster_psram_free(ctx->img);
		ctx->img = NULL;
		return -EINVAL;
	}

	uint32_t stack_size = rec->mem_stack ? rec->mem_stack : 65536;
	uint32_t heap_size = rec->mem_heap ? rec->mem_heap : 262144;
	if (stack_size > 512 * 1024) {
		stack_size = 512 * 1024;
	}
	if (heap_size > 4 * 1024 * 1024) {
		heap_size = 4 * 1024 * 1024;
	}

	ctx->module_inst = wasm_runtime_instantiate(ctx->module, stack_size, heap_size,
						   error_buf, sizeof(error_buf));
	if (!ctx->module_inst) {
		LOG_ERR("instantiate: %s", error_buf);
		return -ENOMEM;
	}
	ctx->exec_env = wasm_runtime_create_exec_env(ctx->module_inst, stack_size);
	if (!ctx->exec_env) {
		LOG_ERR("create_exec_env failed");
		return -ENOMEM;
	}

	ctx->f_init = wasm_runtime_lookup_function(ctx->module_inst, "aster_app_init");
	ctx->f_event = wasm_runtime_lookup_function(ctx->module_inst, "aster_app_event");
	ctx->f_start = wasm_runtime_lookup_function(ctx->module_inst, "aster_app_start");
	ctx->f_pause = wasm_runtime_lookup_function(ctx->module_inst, "aster_app_pause");
	ctx->f_resume = wasm_runtime_lookup_function(ctx->module_inst, "aster_app_resume");
	ctx->f_stop = wasm_runtime_lookup_function(ctx->module_inst, "aster_app_stop");
	ctx->f_destroy = wasm_runtime_lookup_function(ctx->module_inst, "aster_app_destroy");

	if (!ctx->f_init || !ctx->f_event) {
		LOG_ERR("app missing aster_app_init/aster_app_event exports");
		return -EINVAL;
	}

	ctx->is_aot = (want_aot);
	ctx->mem_max = rec->mem_max ? rec->mem_max : 4u * 1024u * 1024u;
	return 0;
}

static int call_fn(aster_app_ctx_t *ctx, void *func, uint32_t argc, uint32_t *ret)
{
	if (!func) {
		return 0;
	}
	uint32_t argv[2] = { 0, 0 };
	if (!wasm_runtime_call_wasm(ctx->exec_env, func, argc, argv)) {
		const char *ex = wasm_runtime_get_exception(ctx->module_inst);
		if (ctx->exit_clean) {
			/* aster_app_exit() -> wasm_runtime_terminate(): expected */
			LOG_DBG("wasm call ended by aster_app_exit: %s", ex ? ex : "?");
		} else {
			LOG_ERR("trap in wasm call: %s", ex ? ex : "?");
		}
		wasm_runtime_clear_exception(ctx->module_inst);
		return -EIO;
	}
	if (ret) {
		*ret = argv[0];
	}
	return 0;
}

/* ---------- event loop ---------- */

static void deliver_event(aster_app_ctx_t *ctx, const aster_event_t *ev)
{
	void *native = NULL;
	uint32_t offset = (uint32_t)wasm_runtime_module_malloc(ctx->module_inst, sizeof(*ev),
							      &native);
	if (!offset || !native) {
		return; /* app OOM: drop event */
	}
	memcpy(native, ev, sizeof(*ev));

	uint32_t argv[1] = { offset };
	if (!wasm_runtime_call_wasm(ctx->exec_env, ctx->f_event, 1, argv)) {
		const char *ex = wasm_runtime_get_exception(ctx->module_inst);
		wasm_runtime_clear_exception(ctx->module_inst);
		wasm_runtime_module_free(ctx->module_inst, offset);
		/* aster_app_exit aborts the in-flight call: that is a clean stop */
		if (!ctx->exit_clean) {
			LOG_ERR("ASTER_TRAP %s: %s", ctx->id, ex ? ex : "?");
			ctx->crashed = true;
		}
		ctx->quit = true;
		return;
	}
	wasm_runtime_module_free(ctx->module_inst, offset);
}

/* pending lifecycle callbacks to run on the app thread (WAMR calls must
 * stay on the app thread): set by shell, consumed here. */
#define ASTER_CTRL_NONE   0
#define ASTER_CTRL_PAUSE  1
#define ASTER_CTRL_RESUME 2

/* Drain platform key edges into ASTER_EVENT_KEY (edge stream: pressed 1/0). */
static void drain_keys(aster_app_ctx_t *ctx)
{
	aster_key_evt_t kev[ASTER_PLAT_MAX_KEYS];
	int n = aster_plat_input_keys_poll(kev, ASTER_PLAT_MAX_KEYS);
	for (int i = 0; i < n && !ctx->quit; i++) {
		/* Global exit key (platform INPUT_KEY_* code, see Kconfig):
		 * intercepted BEFORE delivery. A press cleanly quits the
		 * foreground app; the normal launch policy then relaunches the
		 * system UI. The home UI itself is never force-quit (else the
		 * exit key would strand the shell with a black panel). Both the
		 * press and the matching release are consumed, so the app never
		 * sees a half edge. */
		if (CONFIG_ASTER_EXIT_KEY_CODE && kev[i].code == CONFIG_ASTER_EXIT_KEY_CODE) {
			if (kev[i].pressed &&
			    strcmp(ctx->id, CONFIG_ASTER_SYSTEM_UI) != 0) {
				LOG_INF("exit key: force-quitting %s", ctx->id);
				ctx->exit_clean = true;
				ctx->quit = true;
			}
			continue;
		}
		aster_event_t ev = { 0 };
		ev.size = sizeof(ev);
		ev.version = 1;
		ev.type = ASTER_EVENT_KEY;
		ev.timestamp_ms = (uint64_t)k_uptime_get();
		ev.u.key.code = kev[i].code;
		ev.u.key.pressed = kev[i].pressed;
		deliver_event(ctx, &ev);
	}
}

/* The UI service for the app context: the gesture recognizer, the
 * quick-settings sheet, and the two things LVGL needs to stay alive while a
 * guest owns the screen (the indev samples and the timer pump).
 *
 * IT RUNS ABOVE THE `paused` CHECK, and that is the whole reason it is a
 * function. The paused branch is `k_msleep(20); continue;` -- it skips the
 * entire input path, so anything living below it stops existing the moment
 * an app is paused. The touch poll used to live there, which was fine while
 * nothing but the guest cared; now the sheet and the exit gesture depend on
 * it, and a parked app would look like "the panel is broken" and "pull-up
 * does nothing". Only the two DELIVERY points are still gated on `paused`,
 * because those are the ones that call back into wasm.
 *
 * Thread: the same one that runs aster_launcher_pump_idle() (one worker
 * thread, two phases -- the launcher only pumps while no app owns it), so
 * all of this stays single-threaded.
 *
 * Both call sites (frame step 0 and the pacing slice) go through here, not
 * through a second copy of the body: the touch ring is destructive, so two
 * spellings of "poll it" are two consumers stealing samples from each
 * other. */
static void ui_service(aster_app_ctx_t *ctx)
{
#if IS_ENABLED(CONFIG_ASTER_LVGL)
	aster_touch_evt_t tev[ASTER_TOUCH_MAX];
	int n = aster_plat_input_poll(tev, ASTER_TOUCH_MAX);

	for (int i = 0; i < n && !ctx->quit; i++) {
		bool pressed = tev[i].action != ASTER_TOUCH_UP;
		/* Asked per sample, not once per batch: a pull-down earlier in
		 * this same batch can have just opened or closed the sheet, and
		 * that changes what the next sample is allowed to mean. */
		/* The runtime is deliberately not devicetree-aware, so it asks
		 * the display substrate for the same logical height the touch
		 * coordinates are in -- the launcher passes the compile-time
		 * equivalent (ASTER_UI_H); both are the one number. */
		aster_gest_t g = aster_gesture_feed(tev[i].x, tev[i].y, pressed,
						aster_qpanel_allow(false),
						(int32_t)aster_ui_lvgl_height());

		/* Unconditional, gestures included: LVGL's indev only ever sees
		 * press/release pixels, and a sample it never receives leaves it
		 * believing the finger is still down -- the next tap on one of
		 * the sheet's controls would then read as a drag. */
		aster_ui_lvgl_pointer(tev[i].x, tev[i].y, pressed);

		if (g != ASTER_GEST_NONE) {
			/* Whatever the gesture turns into, it must not also land
			 * as a CLICKED on the widget the finger crossed. */
			lv_indev_wait_release(aster_ui_lvgl_indev());
			/* Everything reaching this point is a guest: the native
			 * launcher is not an app ctx, it pumps on the branch
			 * above. So the sheet is always over an app here --
			 * which is the case that has to save the glass. */
			if (aster_qpanel_on_gesture(g, ASTER_QPANEL_OVER_APP)) {
				LOG_INF("runtime: pull-down (%d,%d) -> panel %s",
					(int)tev[i].x, (int)tev[i].y,
					aster_qpanel_is_open() ? "open" : "closed");
				continue;
			}
			if (g == ASTER_GEST_PANEL_UP) {
				/* Pull-up quits the app. Same route as the exit
				 * key in drain_keys(): set the flags and let the
				 * loop fall out. Deliberately NOT a synthesised
				 * key event -- the app never asked for one, and
				 * inventing an input it did not receive is how a
				 * "quit gesture" turns into a stray menu. */
				LOG_INF("runtime: pull-up (%d,%d) -> quitting %s",
					(int)tev[i].x, (int)tev[i].y, ctx->id);
				ctx->exit_clean = true;
				ctx->quit = true;
				continue;
			}
		}

		/* Nothing is delivered to the guest while the sheet is up: it
		 * owns the input, and a tap that lands on one of its rows must
		 * not also press whatever the app has underneath.
		 *
		 * Note what this does NOT do: repair the guest's press state.
		 * The recogniser only claims a stroke from the sample that
		 * crosses its threshold, so for a pull-down the guest has
		 * already been handed the DOWN and now never gets its UP. That
		 * is left alone on purpose -- the guest's symptom is its own
		 * highlight staying lit until the next touch, which heals
		 * itself, whereas a synthetic UP would be a real activation at
		 * a point the user never tapped. */
		if (ctx->paused || aster_qpanel_is_open()) {
			continue;
		}
		aster_event_t ev = { 0 };
		ev.size = sizeof(ev);
		ev.version = 1;
		ev.type = ASTER_EVENT_TOUCH;
		ev.timestamp_ms = (uint64_t)k_uptime_get();
		ev.u.touch.x = tev[i].x;
		ev.u.touch.y = tev[i].y;
		ev.u.touch.action = tev[i].action;
		/* The guest's surface is in APP space, not the panel's (see
		 * aster_plat_display_app_space): on a board that scales, the
		 * picture it drew is magnified to fill the glass, so a panel
		 * coordinate is not an app coordinate. Everything above this
		 * line -- gestures, the sheet's indev, the qpanel -- deliberately
		 * stays in panel space, because all of it is firmware UI drawn
		 * over the whole glass. A false return means there is no app
		 * pixel under the finger; see aster_plat_display_app_touch(). */
		if (!aster_plat_display_app_touch(&ev.u.touch.x, &ev.u.touch.y)) {
			continue;
		}
		deliver_event(ctx, &ev);
	}

	/* Keys go straight into wasm, so they keep the old gate: an app that
	 * asked to be paused should not have its entry functions called behind
	 * its back. The exit key is the one casualty, and it was already
	 * unreachable on a paused app before this refactor. */
	if (!ctx->paused) {
		drain_keys(ctx);
	}

	/* The sheet is firmware LVGL, but nothing else advances the cpfont
	 * pipeline while an app owns the worker: aster_ui_fonts_poll() lives in
	 * aster_launcher_pump_idle(), and worker_fn() runs app_event_loop()
	 * *instead* of that. So a miss-queued glyph stayed in staging forever
	 * and the sheet drew blank CJK -- same string, some codepoints present
	 * (cached from the launcher) and some not. App-side spelling only:
	 * acking here would eat the guest's own ABI 3.1 repaint request. */
	if (aster_ui_fonts_advance()) {
		lv_obj_t *scr = aster_ui_lvgl_screen();

		if (scr) {
			lv_obj_invalidate(scr);
		}
	}

	/* Keeps the LVGL timer alive while an app owns the worker: it services
	 * the screenshot poll (the shell blocks on it) and drains async flush
	 * waits. The screen tree holds the sheet and nothing else, so no
	 * launcher pixels leak from this pump. */
	aster_ui_lvgl_pump();
#else
	/* Headless: no gestures, no sheet, no LVGL -- just the guest's own
	 * input, and therefore nothing worth doing while it is parked. The
	 * early return reproduces the old placement below the paused check,
	 * which the LVGL build deliberately does not. */
	if (ctx->paused) {
		return;
	}
	aster_touch_evt_t tev[ASTER_TOUCH_MAX];
	int n = aster_plat_input_poll(tev, ASTER_TOUCH_MAX);

	for (int i = 0; i < n && !ctx->quit; i++) {
		aster_event_t ev = { 0 };
		ev.size = sizeof(ev);
		ev.version = 1;
		ev.type = ASTER_EVENT_TOUCH;
		ev.timestamp_ms = (uint64_t)k_uptime_get();
		ev.u.touch.x = tev[i].x;
		ev.u.touch.y = tev[i].y;
		ev.u.touch.action = tev[i].action;
		/* Same app-space mapping as the LVGL branch above; here there is
		 * no firmware UI to keep apart from it. */
		if (!aster_plat_display_app_touch(&ev.u.touch.x, &ev.u.touch.y)) {
			continue;
		}
		deliver_event(ctx, &ev);
	}
	drain_keys(ctx);
#endif
}

static void app_event_loop(aster_app_ctx_t *ctx)
{
	/* ctx->start_ms is set by the caller, before aster_app_init -- see the
	 * comment there. It is NOT set here: by the time this loop runs the app
	 * has already been given control and may already have taken a baseline. */
	uint32_t frame_period = ctx->framerate ? (1000 / ctx->framerate) : 33;

	while (!ctx->quit) {
		uint64_t loop_start = (uint64_t)k_uptime_get();

		/* lifecycle callbacks (pause/resume) run here, not on shell thread */
		switch (ctx->pending_ctrl) {
		case ASTER_CTRL_PAUSE:
			call_fn(ctx, ctx->f_pause, 0, NULL);
			ctx->pending_ctrl = ASTER_CTRL_NONE;
			ctx->paused = true;
			continue;
		case ASTER_CTRL_RESUME:
			ctx->paused = false;
			call_fn(ctx, ctx->f_resume, 0, NULL);
			ctx->pending_ctrl = ASTER_CTRL_NONE;
			break;
		default:
			break;
		}

		/* 0) input + gestures + the sheet. ABOVE the paused check on
		 * purpose -- see ui_service(). Below it, a paused app would
		 * also have a dead panel, a dead pull-up, and an LVGL timer
		 * that stops draining the shell's screenshot poll. */
		ui_service(ctx);

		if (ctx->paused) {
			k_msleep(20);
			continue;
		}

		/* 2) timers */
		for (int i = 0; i < ASTER_TIMER_MAX && !ctx->quit; i++) {
			aster_timer_t *t = &ctx->timers[i];
			if (t->used && t->next_ms && (uint64_t)k_uptime_get() >= t->next_ms) {
				aster_event_t ev = { 0 };
				ev.size = sizeof(ev);
				ev.version = 1;
				ev.type = ASTER_EVENT_TIMER;
				ev.timestamp_ms = (uint64_t)k_uptime_get();
				ev.u.timer.id = ASTER_HTYPE_TIMER | (uint32_t)(i + 1);
				ev.u.timer.user = t->user;
				deliver_event(ctx, &ev);
				if (t->repeat) {
					t->next_ms = (uint64_t)k_uptime_get() + t->period_ms;
				} else {
					t->used = false;
				}
			}
		}

		/* 3) frame */
		if (!ctx->quit) {
			aster_event_t ev = { 0 };
			ev.size = sizeof(ev);
			ev.version = 1;
			ev.type = ASTER_EVENT_FRAME;
			ev.timestamp_ms = (uint64_t)k_uptime_get();
			ev.u.frame.frame = ctx->frame_no++;
			deliver_event(ctx, &ev);
		}

		/* memory stats (runtime pool, not wasm linear: proxy) */
		mem_alloc_info_t mai;
		wasm_runtime_get_mem_alloc_info(&mai);
		aster_app_mgr_note_mem(ctx->id, mai.total_size - mai.total_free_size);

		/* Pace to the framerate, but slice the sleep so touch latency
		 * stays low: a single k_msleep(frame) would make the runtime
		 * sample input only every ~33 ms and drop move points that the
		 * (faster) driver poll reports in between — finger strokes
		 * arrive as sparse DOWN/UP pairs and paint draws nothing.
		 * Drain input every ~5 ms within the remaining frame budget. */
		uint64_t spent = (uint64_t)k_uptime_get() - loop_start;
		uint32_t period = ctx->framerate ? (1000 / ctx->framerate) : frame_period;
		int64_t remain = (int64_t)period - (int64_t)spent;

		/* Floor the sleep at one tick even when the frame overran its
		 * budget. This is a single-core part (CONFIG_MP_MAX_NUM_CPUS=1)
		 * and the worker runs preemptively, so a render that takes
		 * longer than the period would otherwise drive the loop with
		 * no yield at all: every thread at a lower priority -- the
		 * shell above all -- stops getting scheduled and the device
		 * looks wedged until the app exits (no `aster app stop`, no SD
		 * access). 1 ms is ~4% of a 26 ms frame and buys back a
		 * responsive system. */
		if (remain <= 0) {
			remain = 1;
		}
		while (remain > 0 && !ctx->quit) {
			int64_t slice = remain < 5 ? remain : 5;
			k_sleep(K_MSEC((int32_t)slice));
			remain -= slice;

			/* The SAME service, not a second copy of its body: this
			 * slice is the low-latency half of the loop and it has to
			 * see exactly the same input path as step 0. Two spellings
			 * of "poll the touch ring" is two places for the recognizer
			 * to be fed from -- and the ring is destructive, so a
			 * second consumer would silently steal samples from the
			 * first. */
			ui_service(ctx);
		}
		if (remain <= 0 && spent == 0) {
			k_yield();
		}
	}
}

static void ctx_cleanup(aster_app_ctx_t *ctx)
{
	if (!ctx) {
		return;
	}
#if IS_ENABLED(CONFIG_ASTER_LVGL)
	aster_gfx_drop_all(); /* free sprite-cache slots held by this app */
#endif
	aster_native_release_files(ctx);

	if (ctx->exec_env) {
		wasm_runtime_destroy_exec_env(ctx->exec_env);
		ctx->exec_env = NULL;
	}
	if (ctx->module_inst) {
		wasm_runtime_deinstantiate(ctx->module_inst);
		ctx->module_inst = NULL;
	}
	if (ctx->module) {
		wasm_runtime_unload(ctx->module);
		ctx->module = NULL;
	}
	if (ctx->img) {
		aster_psram_free(ctx->img);
		ctx->img = NULL;
	}
}

/* Load + init + start the pending app. Runs on the worker thread (all WAMR
 * calls are bound to it). Returns ASTER_OK once the app is ready to run its
 * event loop; on failure everything allocated so far is reclaimed. */
static int ctx_startup(aster_app_ctx_t *ctx)
{
    /* A package can remain installed when the display mode changes. Recheck
     * its requirements on the worker after applying a queued mode change. */
    aster_manifest_t manifest;
    int manifest_err = aster_app_mgr_read_manifest(ctx->id, &manifest);
    if (manifest_err) return manifest_err;
    for (uint32_t i = 0; i < manifest.requires_n; ++i) {
        if (!aster_sysinfo_feature_by_name(manifest.requires[i])) {
            LOG_ERR("start: capability unavailable in current mode: %s", manifest.requires[i]);
            return ASTER_ENOTSUP;
        }
    }
	aster_app_rec_t *rec = ctx->rec; /* ctx_load may downgrade has_aot */

	int err = ctx_load(ctx, rec);
	if (err) {
		/* ctx_load returns with img/module/inst partially acquired on
		 * every failure path; without this the PSRAM pool bleeds a few
		 * hundred KB per failed start (observed: transient exec_env
		 * failure -> 15x start/stop cascade -> pool exhausted). */
		ctx_cleanup(ctx);
		return ASTER_ECORRUPT;
	}

	/* The app's clock starts when the app does -- here, not in
	 * app_event_loop(). aster_time_now_ms() is specified as "ms since this app
	 * started" (abi-v1.md), and an app's natural place to take a baseline is
	 * aster_app_init. With start_ms still 0 during init, that first call
	 * returned the DEVICE uptime instead; the baseline was then ~700000 ms
	 * while every later call was app-relative, so the first delta computed
	 * against it wrapped to ~4.29e9. Observed on gfxshow: `fps=0
	 * t=4294259866` on the first report of every run, every later report
	 * correct -- no error, no clamp, just one absurd interval at the top. */
	ctx->start_ms = (uint64_t)k_uptime_get();

	uint32_t init_ret = 0;
	if (call_fn(ctx, ctx->f_init, 0, &init_ret) != 0) {
		if (!ctx->exit_clean) {
			LOG_ERR("aster_app_init trapped");
			ctx_cleanup(ctx);
			return ASTER_ECORRUPT;
		}
		ctx->quit = true; /* exited from within init: skip straight to teardown */
		return ASTER_OK;
	}
	if (init_ret != 0) {
		LOG_ERR("aster_app_init refused (%d)", (int)init_ret);
		ctx_cleanup(ctx);
		return ASTER_ESTATE;
	}

	if (call_fn(ctx, ctx->f_start, 0, NULL) != 0 && !ctx->exit_clean) {
		ctx_cleanup(ctx);
		return ASTER_ECORRUPT;
	}
	if (ctx->exit_clean) {
		ctx->quit = true;
	}
	return ASTER_OK;
}

static void worker_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	for (;;) {
        apply_display_mode();
        if (k_sem_take(&worker_go, K_MSEC(10))) {
#if IS_ENABLED(CONFIG_ASTER_LAUNCHER_NATIVE)
            aster_launcher_pump_idle();
#endif
            continue;
        }
#if IS_ENABLED(CONFIG_ASTER_LAUNCHER_NATIVE)
        aster_launcher_hide();
#endif
		aster_app_ctx_t *ctx = worker_pending;
		worker_pending = NULL;

		/* The launcher is down and this is the thread that owns LVGL:
		 * the one moment the window can be turned without tearing a
		 * frame that is mid-flush. Before ctx_startup, so the app's
		 * aster_app_init sees the space it asked for. */
		orient_lease_take(ctx->rec);

		/* publish the owner *before* running aster_app_init: native
		 * calls (surface/fs) resolve ctx_of() during init too */
		current = ctx;
		worker_result = ctx_startup(ctx);
		if (worker_result != ASTER_OK) {
			current = NULL;
			/* the ctx never ran: hand the record back, aster_app_start
			 * published it as RUNNING before the handoff */
			aster_app_mgr_set_state(ctx->id, ASTER_APP_STATE_STOPPED, 0);
			/* this path skips the teardown below, and it is the
			 * launcher -- not another app -- that comes back, so
			 * the window has to be put back here too. */
			orient_lease_release();
			k_free(ctx);
			k_sem_give(&worker_ready);
			continue;
		}

		if (!ctx->quit) {
			ctx->state = ASTER_APP_STATE_RUNNING;
			aster_app_mgr_set_state(ctx->id, ASTER_APP_STATE_RUNNING, 0);
			aster_app_mgr_note_start(ctx->id, (uint64_t)k_uptime_get());
			LOG_INF("app started: %s (%s, %u KB, %s)", ctx->id, ctx->rec->name,
				(unsigned)(ctx->img_size / 1024), ctx->is_aot ? "AOT" : "wasm");
		}
		k_sem_give(&worker_ready);

		app_event_loop(ctx);

		/* teardown (wasm calls must run on this thread) */
		call_fn(ctx, ctx->f_stop, 0, NULL);
		call_fn(ctx, ctx->f_destroy, 0, NULL);

		char id[64];
		strncpy(id, ctx->id, sizeof(id) - 1);
		id[sizeof(id) - 1] = 0;
		bool crashed = ctx->crashed;
		ctx_cleanup(ctx);
		k_free(ctx);

#if IS_ENABLED(CONFIG_ASTER_LVGL)
		/* The app under the sheet is gone -- stopped, crashed, or pulled
		 * up -- and the sheet has to go WITH it, non-restoring: its saved
		 * frame is this app's last picture, and painting that over the
		 * launcher that is about to replace it would be a ghost. Also
		 * clears the freeze flag; left armed, the next app would start
		 * with every present already swallowed (a black screen with no
		 * obvious cause). Here rather than in the loop above because the
		 * `continue`s there skip this -- and before relaunch_policy_after
		 * so the launcher is not raised with a stale sheet on top of it. */
		aster_qpanel_on_app_stop();
#endif

		aster_app_mgr_set_state(id, crashed ? ASTER_APP_STATE_CRASHED
					    : ASTER_APP_STATE_STOPPED, 0);
		if (crashed) {
			LOG_ERR("app crashed: %s (resources reclaimed)", id);
		}

		/* Put the window back before anything is drawn on it again --
		 * the launcher below is laid out for the build-time window and
		 * does not reflow. Before the give: a shell `aster app stop`
		 * returns on worker_done and can start the next app's lease
		 * immediately. */
		orient_lease_release();

		current = NULL;
		k_sem_give(&worker_done);

		/* ABI 1.2 launch policy: handoff target or home UI, deferred
		 * to sysworkq (worker cannot launch itself). After the give so
		 * a pending aster_app_stop returns first and its sem-reset in
		 * aster_app_start cannot race our teardown. */
		relaunch_policy_after(id);
	}
}

int aster_app_start(const char *id)
{
	if (!runtime_inited) {
		return ASTER_ESTATE;
	}
	if (!aster_storage_mounted()) {
		return ASTER_EIO;
	}

	k_mutex_lock(&app_lock, K_FOREVER);
	int ret = ASTER_OK;

	if (current || worker_pending) {
		ret = ASTER_EBUSY; /* V1: one app at a time */
		goto out;
	}
	const aster_app_rec_t *rec = aster_app_mgr_find(id);
	if (!rec) {
		ret = ASTER_ENOENT;
		goto out;
	}
	if (rec->state == ASTER_APP_STATE_RUNNING || rec->state == ASTER_APP_STATE_PAUSED) {
		ret = ASTER_EBUSY;
		goto out;
	}

	aster_app_ctx_t *ctx = k_calloc(1, sizeof(*ctx));
	if (!ctx) {
		ret = ASTER_ENOMEM;
		goto out;
	}
	strncpy(ctx->id, id, sizeof(ctx->id) - 1);
	ctx->framerate = 30;
	/* find() hands back a const view because the record is app_mgr's, but
	 * the runtime keeps a writable handle: ctx_load downgrades has_aot when
	 * a corrupt AOT image forces the wasm fallback (see ctx_load, and
	 * aster_app_mgr.c's own has_aot writes). Cast documents that intent. */
	ctx->rec = (aster_app_rec_t *)rec;

	/* Publish the record as RUNNING *before* the handoff: from here the
	 * worker owns the ctx -- and reads ctx->rec -- while it loads the
	 * image, so a record left at STOPPED would let a concurrent
	 * `aster app uninstall` delete the app directory mid-load and compact
	 * recs[] under ctx->rec. The manager keys its uninstall guard and its
	 * rescan preservation off this state, so it must cover the whole ctx
	 * lifetime: worker_fn's startup-failure path hands it back to STOPPED
	 * and teardown sets CRASHED/STOPPED once the ctx is gone. */
	aster_app_mgr_set_state(id, ASTER_APP_STATE_RUNNING, 0);

	/* hand the whole launch to the worker: load -> init -> start.
	 * `current` is set there before wasm executes, so aster_app_exit and
	 * owner lookups work even inside aster_app_init. */
	k_sem_reset(&worker_done); /* drop counts from a prior self-exit */
	worker_pending = ctx;
	k_sem_give(&worker_go);
	if (k_sem_take(&worker_ready, K_MSEC(15000))) {
		LOG_ERR("worker wedged launching %s", id);
		ret = ASTER_ETIMEDOUT;
		goto out;
	}
	ret = worker_result;
out:
	k_mutex_unlock(&app_lock);
	return ret;
}

int aster_app_stop(const char *id)
{
	k_mutex_lock(&app_lock, K_FOREVER);
	aster_app_ctx_t *ctx = current;
	int ret = ASTER_OK;

	if (!ctx || strcmp(ctx->id, id) != 0) {
		ret = ASTER_ENOENT;
		goto out;
	}
	ctx->quit = true;
	ctx->paused = false;
	ctx->pending_ctrl = ASTER_CTRL_NONE;
	if (k_sem_take(&worker_done, K_MSEC(5000))) {
		/* worker stuck inside wasm: app stays marked; resources leak
		 * until reboot but the shell remains usable */
		LOG_ERR("app %s did not stop in time", id);
		ret = ASTER_ETIMEDOUT;
		goto out;
	}
	LOG_INF("app stopped: %s", id);
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	(void)aster_plat_audio_stop();
#endif
out:
	k_mutex_unlock(&app_lock);
	return ret;
}

int aster_app_pause(const char *id)
{
	k_mutex_lock(&app_lock, K_FOREVER);
	aster_app_ctx_t *ctx = current;
	int ret = ASTER_OK;
	/* id not the running app -> ENOENT (same as stop); id matches but it
	 * is not RUNNING -> ESTATE (e.g. double pause). */
	if (!ctx || strcmp(ctx->id, id) != 0) {
		ret = ASTER_ENOENT;
		goto out;
	}
	if (ctx->state != ASTER_APP_STATE_RUNNING) {
		ret = ASTER_ESTATE;
		goto out;
	}
	ctx->pending_ctrl = ASTER_CTRL_PAUSE; /* consumed on app thread */
	ctx->state = ASTER_APP_STATE_PAUSED;
	aster_app_mgr_set_state(id, ASTER_APP_STATE_PAUSED, 0);
	LOG_INF("app paused: %s", id);
out:
	k_mutex_unlock(&app_lock);
	return ret;
}

int aster_app_resume(const char *id)
{
	k_mutex_lock(&app_lock, K_FOREVER);
	aster_app_ctx_t *ctx = current;
	int ret = ASTER_OK;
	if (!ctx || strcmp(ctx->id, id) != 0) {
		ret = ASTER_ENOENT;
		goto out;
	}
	if (ctx->state != ASTER_APP_STATE_PAUSED) {
		ret = ASTER_ESTATE;
		goto out;
	}
	ctx->pending_ctrl = ASTER_CTRL_RESUME; /* consumed on app thread */
	ctx->state = ASTER_APP_STATE_RUNNING;
	aster_app_mgr_set_state(id, ASTER_APP_STATE_RUNNING, 0);
	LOG_INF("app resumed: %s", id);
out:
	k_mutex_unlock(&app_lock);
	return ret;
}

aster_app_ctx_t *aster_app_current(void)
{
	return current;
}

/* ---------- system-UI launch policy (ABI 1.2) ----------
 * Two relaunch paths run after an app fully tears down (worker thread):
 *  - handoff: the exiting app was itself the launcher and called
 *    aster_appmgr_start(target) -> launch target immediately;
 *  - home: any other app exits (clean, self-stop or crash) and the board
 *    configures a system UI -> re-launch CONFIG_ASTER_SYSTEM_UI so the device
 *    is never UI-less after boot. The UI itself exiting is final (no loop).
 * The launch is deferred to the system workqueue because aster_app_start
 * hands the worker via semaphores and the worker cannot wait on itself. */

static void relaunch_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	char id[64];

	/* take the id only; aster_app_start re-takes app_lock itself (mutexes
	 * are not reentrant), so we must not hold it across the call */
	k_mutex_lock(&app_lock, K_FOREVER);
	strncpy(id, relaunch_id, sizeof(id));
	relaunch_id[0] = 0;
	k_mutex_unlock(&app_lock);

	if (!id[0]) {
		return;
	}
	int ret = aster_app_start(id);
	if (ret != ASTER_OK) {
		LOG_WRN("policy launch %s: %d", id, ret);
	}
}

int aster_runtime_handoff(const char *id)
{
	if (!current) {
		return ASTER_ESTATE;
	}
	if (!strcmp(current->id, id)) {
		return ASTER_EINVAL;
	}
	if (!aster_app_mgr_find(id)) {
		return ASTER_ENOENT;
	}
	k_mutex_lock(&app_lock, K_FOREVER);
	strncpy(relaunch_id, id, sizeof(relaunch_id) - 1);
	relaunch_id[sizeof(relaunch_id) - 1] = 0;
	current->quit = true;
	k_mutex_unlock(&app_lock);
	return ASTER_OK;
}

/* Built-in launcher card tap (worker thread, no running app): launch `id`
 * after this callback returns. The worker cannot aster_app_start itself (it
 * would wait on its own sems), so defer to the sys workqueue exactly like
 * the handoff/home policy. */
int aster_runtime_launch_async(const char *id)
{
	if (!runtime_inited || !id || !id[0]) {
		return ASTER_EINVAL;
	}
	if (!aster_app_mgr_find(id)) {
		return ASTER_ENOENT;
	}
	k_mutex_lock(&app_lock, K_FOREVER);
	strncpy(relaunch_id, id, sizeof(relaunch_id) - 1);
	relaunch_id[sizeof(relaunch_id) - 1] = 0;
	k_mutex_unlock(&app_lock);
	k_work_submit(&relaunch_work);
	return ASTER_OK;
}

void aster_runtime_boot_ui(void)
{
	if (!runtime_inited) {
		return;
	}
#if IS_ENABLED(CONFIG_ASTER_LAUNCHER_NATIVE)
	/* Built-in launcher: the worker thread owns LVGL, so only flag the
	 * request; its idle pump draws home on the next pass (< 10 ms). */
	aster_launcher_request_show();
	LOG_INF("system ui: built-in launcher");
	return;
#else
	if (sizeof(CONFIG_ASTER_SYSTEM_UI) <= 1) {
		return;
	}
	if (!aster_app_mgr_find(CONFIG_ASTER_SYSTEM_UI)) {
		LOG_WRN("system ui %s not installed; shell only",
			CONFIG_ASTER_SYSTEM_UI);
		return;
	}
	k_mutex_lock(&app_lock, K_FOREVER);
	strncpy(relaunch_id, CONFIG_ASTER_SYSTEM_UI, sizeof(relaunch_id) - 1);
	relaunch_id[sizeof(relaunch_id) - 1] = 0;
	k_mutex_unlock(&app_lock);
	k_work_submit(&relaunch_work);
	LOG_INF("system ui: booting %s", CONFIG_ASTER_SYSTEM_UI);
#endif
}

static void relaunch_policy_after(const char *exited_id)
{
	const bool ui_set = sizeof(CONFIG_ASTER_SYSTEM_UI) > 1;

	k_mutex_lock(&app_lock, K_FOREVER);
	bool handoff = relaunch_id[0] != 0; /* aster_appmgr_start set a target */
	if (!handoff && ui_set && strcmp(exited_id, CONFIG_ASTER_SYSTEM_UI) != 0) {
		/* normal app quit -> return home to the system UI */
		strncpy(relaunch_id, CONFIG_ASTER_SYSTEM_UI, sizeof(relaunch_id) - 1);
		relaunch_id[sizeof(relaunch_id) - 1] = 0;
		handoff = true;
	}
	bool submit = handoff && relaunch_id[0] != 0;
	k_mutex_unlock(&app_lock);

	if (submit) {
		k_work_submit(&relaunch_work);
	}
}

/* clean self-exit requested by the app (aster_app_exit): not a crash.
 * wasm_runtime_terminate aborts the in-flight wasm call with a "terminated
 * by user" exception so an app cannot run past aster_app_exit(). */
void aster_runtime_exit(int32_t code)
{
	if (current) {
		current->exit_clean = true;
		current->exit_code = (uint32_t)code;
		current->quit = true;
		if (current->module_inst) {
			wasm_runtime_terminate(current->module_inst);
		}
	}
}

/* legacy hook: native code aborting the app == clean exit */
void aster_runtime_terminate(void)
{
	aster_runtime_exit(0);
}
