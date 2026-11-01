/* SPDX-License-Identifier: MIT */
#pragma once
/* Single VM owner. Raw diagnostics stop after 600 frames; installed apps do not. */
int aster_idf_run_wasm(const char *path);
/* Queue owner clears stop before publication, so an intervening stop survives. */
int aster_idf_run_queued_diagnostic(const char *path);

#include <stdint.h>
#include <stdbool.h>
/* Call before starting the VM; callbacks run synchronously on its owning task.
 * present must finish reading guest pixels before returning. */
#define ASTER_IDF_MAX_TOUCH 5
typedef struct { uint32_t x, y; } aster_idf_touch_point_t;
typedef struct {
    uint32_t width, height;
    int (*present)(const uint8_t *pixels, uint32_t x, uint32_t y,
                   uint32_t width, uint32_t height, uint32_t pitch_pixels);
    /* 1 contact, 0 released, negative I/O failure; failure preserves last state. */
    int (*touch)(uint32_t *x, uint32_t *y);
    /* Fixed hardware slots; returns active bitmask, negative = no valid frame. */
    int (*touch_frame)(aster_idf_touch_point_t points[ASTER_IDF_MAX_TOUCH]);
    uint32_t max_touch_points;
} aster_idf_io_t;
void aster_idf_set_io(const aster_idf_io_t *io);

void aster_idf_request_stop(void);
int aster_idf_request_pause(bool pause);
int aster_idf_run_installed(const char *path,const char *id,uint32_t permissions);

void aster_idf_clear_stop(void);

bool aster_idf_is_running(void);
bool aster_idf_stop_is_requested(void);
void aster_idf_check_watchdog(void);
void aster_idf_exit_current(int code);
bool aster_idf_take_handoff(char out[64]);
typedef struct {
    bool (*touch)(void *event);
    void (*tick)(void);
    bool (*holds_glass)(void);
} aster_idf_ui_hooks_t;
void aster_idf_set_ui_hooks(const aster_idf_ui_hooks_t *hooks);
bool aster_idf_app_glass_frozen(void);
void aster_idf_set_capture(const uint8_t *(*capture)(void));
const uint8_t *aster_idf_capture(void);
