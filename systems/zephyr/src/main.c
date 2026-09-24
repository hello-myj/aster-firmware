#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * ASTER firmware entry point.
 *
 * Boot order: core mem -> platform (display/backlight/input) -> storage
 * (SD mount) -> services (sysinfo/apps/kv) -> WAMR runtime -> shell.
 */
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <aster/aster.h>
#include "aster_mem.h"
#include "aster_logring.h"
#include "aster_storage.h"
#include "aster_app_mgr.h"
#include "aster_sysinfo.h"
#include "aster_platform.h"
#include "aster_runtime.h"
#include "aster_file_svc.h"
#if IS_ENABLED(CONFIG_ASTER_LVGL)
#include "ui_lvgl.h"
#endif

LOG_MODULE_REGISTER(aster_main, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("ASTER system v%s (ABI %u.%u.%u) booting", ASTER_SYSVER,
		ASTER_ABI_MAJOR, ASTER_ABI_MINOR, ASTER_ABI_PATCH);

	aster_core_init();

	/* after core mem: the ring lives in the PSRAM shared heap */
	if (aster_logring_init()) {
		LOG_WRN("log ring alloc failed; `aster log` will stay empty");
	}

	int err = aster_platform_init();
	if (err) {
		LOG_WRN("platform init: %d (continuing)", err);
	}

#if IS_ENABLED(CONFIG_ASTER_LVGL)
	err = aster_ui_lvgl_init();
	if (err) {
		LOG_WRN("ui substrate init: %d (continuing)", err);
	}
#endif

	err = aster_storage_init();
	if (err) {
		LOG_WRN("storage init failed: %d (insert SD card)", err);
	} else {
		(void)aster_app_mgr_init();
		LOG_INF("apps registered: %d", aster_app_mgr_count());
	}

	if (IS_ENABLED(CONFIG_ASTER_WAMR)) {
		err = aster_runtime_init();
		if (err) {
			LOG_ERR("runtime init failed: %d", err);
		} else {
            (void)aster_runtime_display_mode_request(
                IS_ENABLED(CONFIG_ASTER_BOOT_HEADLESS) ? ASTER_DISPLAY_MODE_HEADLESS :
                IS_ENABLED(CONFIG_ASTER_BOOT_PORTRAIT) ? ASTER_DISPLAY_MODE_PORTRAIT :
                ASTER_DISPLAY_MODE_LANDSCAPE);
			aster_runtime_boot_ui(); /* CONFIG_ASTER_SYSTEM_UI */
		}
	}

	/* .mja push service on the native USB-CDC port (ttyACM*) */
	err = aster_file_svc_start();
	if (err && err != -ENOTSUP) {
		LOG_WRN("file svc: %d (CDC push disabled)", err);
	}

	LOG_INF("ready — type `aster` or `hw` for commands");
	return 0;
}
