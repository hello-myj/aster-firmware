/* SPDX-License-Identifier: MIT
 * ASTER Runtime: hosts WAMR, one thread per running app (V1: single app),
 * event loop, lifecycle + trap isolation (design doc sec.6/37/44).
 */
#ifndef ASTER_RUNTIME_H
#define ASTER_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "aster_app_mgr.h"

#define ASTER_SURFACE_MAX 4
#define ASTER_TIMER_MAX   8
#define ASTER_FILE_MAX    8
#define ASTER_TOUCH_MAX   2

typedef struct {
	uint32_t app_offset; /* RGB565 buffer inside app linear memory */
	uint32_t size;
	uint32_t w, h, pitch;
	uint32_t format;
	bool     used;
	/* ASTER_SURFACE_F_PANEL_CANVAS was granted: app_offset is 0 and the
	 * pixels live in the platform's front buffer instead, in panel byte
	 * order, drawn through the gfx engine (see aster_plat_display_canvas_*). */
	bool     canvas;
} aster_surface_t;

typedef struct {
	uint32_t period_ms;
	uint32_t repeat;
	uint32_t user;
	uint64_t next_ms;
	bool     used;
} aster_timer_t;

struct fs_file_t;

typedef struct {
	struct fs_file_t *f;
	bool used;
} aster_file_t;

typedef struct aster_app_ctx {
	char id[64];
	aster_app_rec_t *rec; /* app record (stable, owned by app_mgr; ctx_load
			     * may downgrade has_aot on corrupt-AOT fallback) */
	/* WAMR objects */
	void    *module;      /* wasm_module_t  */
	void    *module_inst; /* wasm_module_inst_t */
	void    *exec_env;    /* wasm_exec_env_t */
	uint8_t *img;         /* loaded wasm/aot bytes (PSRAM) */
	size_t   img_size;
	bool     is_aot;
	void    *f_init, *f_event, *f_start, *f_pause, *f_resume, *f_stop, *f_destroy;

	/* resources owned by this app */
	aster_surface_t surfaces[ASTER_SURFACE_MAX];
	aster_timer_t   timers[ASTER_TIMER_MAX];
	aster_file_t    files[ASTER_FILE_MAX];

	volatile uint32_t state; /* aster_app_state_t */
	volatile bool     quit;
	volatile bool     paused;
	volatile bool     crashed;      /* event loop ended via a wasm trap */
	volatile bool     exit_clean;   /* set by aster_app_exit (not a crash) */
	volatile uint32_t exit_code;
	volatile uint32_t pending_ctrl; /* ASTER_CTRL_x, consumed on app thread */
	uint32_t          framerate;
	uint64_t          start_ms;
	uint32_t          frame_no;
	uint32_t          mem_max;

	/* touch state fed to the app on demand */
	uint32_t          touch_pts[ASTER_TOUCH_MAX * 4];
	uint32_t          touch_n;
} aster_app_ctx_t;

enum aster_display_mode {
    ASTER_DISPLAY_MODE_HEADLESS = 0,
    ASTER_DISPLAY_MODE_PORTRAIT = 1,
    ASTER_DISPLAY_MODE_LANDSCAPE = 2,
};
/* Queue a mode change on the UI owner thread; EBUSY while an APP owns it. */
int aster_runtime_display_mode_request(enum aster_display_mode mode);
int  aster_runtime_init(void);

/* Owner lookup from native callbacks: returns ctx if `inst` belongs to the
 * running app, else NULL. */
aster_app_ctx_t *aster_runtime_owner(void *module_inst);

/* control plane (called from shell) */
int  aster_app_start(const char *id);
int  aster_app_stop(const char *id);
int  aster_app_pause(const char *id);
int  aster_app_resume(const char *id);
aster_app_ctx_t *aster_app_current(void);

/* ABI 1.2 handoff (called from appmgr native on the worker thread): marks
 * the current app to quit and arranges `id` to launch after teardown. */
int  aster_runtime_handoff(const char *id);

/* Enqueue `id` for startup on the UI worker without waiting for init.
 * Used by launcher callbacks and the system workqueue launch policy.
 * ASTER_OK confirms enqueueing; startup failures are reported in the log. */
int  aster_runtime_launch_async(const char *id);

/* Boot auto-start of CONFIG_ASTER_SYSTEM_UI (no-op when unset/not installed). */
void aster_runtime_boot_ui(void);

#endif
