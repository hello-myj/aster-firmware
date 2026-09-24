/* SPDX-License-Identifier: MIT */
#ifndef ASTER_APP_H
#define ASTER_APP_H

#include "types.h"
#include "event.h"

/*
 * App lifecycle exports (design doc sec.6). All optional except init+event:
 * the Runtime calls whichever exist.
 *
 *   int  aster_app_init(void)         -> 0 == ASTER_OK to accept launch
 *   void aster_app_start(void)
 *   void aster_app_pause(void)
 *   void aster_app_resume(void)
 *   void aster_app_stop(void)
 *   void aster_app_destroy(void)
 *   void aster_app_event(const aster_event_t *)
 *
 * Implemented in C by the app; exported from the module. Use ASTER_APP_EXPORT
 * (build.py passes --export for each of them).
 */

#if defined(__wasm__)
#define ASTER_APP_EXPORT __attribute__((visibility("default")))
#else
#define ASTER_APP_EXPORT
#endif

/* Ask the Runtime to tear the app down (returns nothing; app is stopped). */
ASTER_I("aster_app_exit") void aster_app_exit(int32_t code);

/* Frame pacing: request N fps for ASTER_EVENT_FRAME (default 30, max 60). */
ASTER_I("aster_app_set_framerate") int32_t aster_app_set_framerate(uint32_t fps);

/* Milliseconds since app start (u64 split into lo/hi halves). */
ASTER_I("aster_time_now_ms") uint32_t aster_time_now_ms(void);

/* Log: level = aster_log_level_t, msg = utf-8 bytes (no NUL needed). */
ASTER_I("aster_log") void aster_log(uint32_t level, const char *msg, uint32_t len);

#endif /* ASTER_APP_H */
