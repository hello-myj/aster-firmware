/* SPDX-License-Identifier: MIT
 * "aster ui screenshot" debug transport (CONFIG_ASTER_SCREENSHOT).
 *
 * The UI thread owns LVGL, so the grab must run there: the shell command
 * flags a request, the lvgl pump services it between frames (glass frame ->
 * miniz zlib -> numbered base64 lines on the shell), and
 * releases the caller when the last line is out. sdk/tools/shell_shot.py
 * reassembles the stream into a PNG.
 */
#ifndef ASTER_UI_SCREENSHOT_H
#define ASTER_UI_SCREENSHOT_H

#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_ASTER_SCREENSHOT)

#include <stdbool.h>
#include <zephyr/shell/shell.h>

/* Called from the LVGL pump (UI/worker thread). Cheap no-op when idle. */
void aster_screenshot_poll(void);

/* Block (caller thread) until the UI thread streams one grab out. */
int aster_screenshot_request(const struct shell *sh);

#endif /* CONFIG_ASTER_SCREENSHOT */
#endif /* ASTER_UI_SCREENSHOT_H */
