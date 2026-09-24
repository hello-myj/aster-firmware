/* SPDX-License-Identifier: MIT
 * Built-in native launcher: LVGL home + app drawer rendered by the
 * firmware itself, not a guest app. Show/hide is a tree rebuild, so
 * returning home costs no app (re)start.
 *
 * All entry points run on the UI/worker thread (serialized with LVGL).
 */
#ifndef ASTER_LAUNCHER_H
#define ASTER_LAUNCHER_H

#include <stdbool.h>

/* Bind the baked rodata fonts (idempotent, once). */
int  aster_launcher_init(void);

/* Build the launcher tree on the active screen (no-op if already shown).
 * Must run on the UI/worker thread (owns LVGL). */
void aster_launcher_show(void);

/* Thread-safe boot request: the worker's idle pump draws home on its next
 * pass. Safe to call from the main/shell thread before/without LVGL. */
void aster_launcher_request_show(void);

/* Thread-safe page switch (0 = home, 1 = drawer) for the debug screenshot
 * CLI: records the want; the idle pump applies it, visual-only (no kv). */
void aster_launcher_show_page(int page);

/* Step the page by `dir` (+1 = next). This one is a gesture on the glass,
 * not a debug poke: it persists, and it is a no-op past either end rather
 * than wrapping. UI thread only. */
void aster_launcher_step_page(int dir);

/* Tear the tree down (no-op if hidden). Safe from any state. */
void aster_launcher_hide(void);

/* Pump one LVGL tick + inject a touch edge while idle (no app running).
 * Called from the runtime worker's idle branch. */
void aster_launcher_pump_idle(void);

/* The app registry changed (install/uninstall/scan): flag the drawer and
 * the resident name-subset for a rebuild on the next pump. Thread-safe. */
void aster_launcher_on_registry_change(void);

#endif /* ASTER_LAUNCHER_H */
