/* SPDX-License-Identifier: MIT
 * Boot splash (font-free loading animation) -- see ui_splash.c.
 */
#ifndef ASTER_UI_SPLASH_H
#define ASTER_UI_SPLASH_H

#include <stdbool.h>
#include <stdint.h>

/* Allocate the band + first paint. Call once after the panel is up. */
void aster_splash_boot(void);

/* Repaint if due (~8 fps) and streaming is still in progress. Cheap no-op
 * when off or before the next frame slot. UI/worker pump only. */
void aster_splash_tick(void);

/* Free the band. Idempotent; called the moment the first real frame is up. */
void aster_splash_off(void);

#endif
