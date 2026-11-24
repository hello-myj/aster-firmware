/* SPDX-License-Identifier: MIT
 * Built-in native launcher: the FLUX ribbon home, rendered by the
 * firmware itself, not a guest app. Show/hide is a tree rebuild, so
 * returning home costs no app (re)start.
 *
 * All entry points run on the UI/worker thread (serialized with LVGL).
 */
#ifndef ASTER_LAUNCHER_H
#define ASTER_LAUNCHER_H

#include <stdbool.h>
#include <stdint.h>

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
#ifdef ASTER_AI_ENABLED
void aster_launcher_open_ai(void);
void aster_launcher_configure_ai(bool online);
bool aster_launcher_ai_online(void);
#include "ai_dialog.h"
void aster_launcher_ask_ai(enum ai_query_source source);
#endif

/* Step the page by `dir` (+1 = next). This one is a gesture on the glass,
 * not a debug poke: it persists, and it is a no-op past either end rather
 * than wrapping. UI thread only. */
void aster_launcher_step_page(int dir);

/* Tear the tree down (no-op if hidden). Safe from any state. */
void aster_launcher_hide(void);

/* Pump one LVGL tick + inject a touch edge while idle (no app running).
 * Called from the runtime worker's idle branch. */
void aster_launcher_pump_idle(void);
uint32_t aster_launcher_idle_wait_ms(void);
/* UI-thread diagnostics: cumulative bakes/hits and detached PSRAM bytes. */
void aster_launcher_art_stats(uint32_t *bakes,uint32_t *hits,uint32_t *cached_bytes);
typedef struct {
 uint32_t presses,releases,changes,saves,max_save_ms,max_set_us;
 int32_t value;
} aster_launcher_slider_stats_t;
void aster_launcher_slider_stats(aster_launcher_slider_stats_t *out);

/* The app registry changed (install/uninstall/scan): flag the drawer and
 * the resident name-subset for a rebuild on the next pump. Thread-safe. */
void aster_launcher_on_registry_change(void);

bool aster_launcher_power_touch(bool contacts_down);
void aster_launcher_power_tick(void);
void aster_launcher_power_resume(void);

#endif /* ASTER_LAUNCHER_H */
