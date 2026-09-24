/* SPDX-License-Identifier: MIT
 * Internals shared by the gfx engine's two rasterizers: the command engine
 * (ui_gfx.c) and the pretty-poly path renderer it delegates the AA shapes to
 * (pp_gfx.c). Both paint the same target, so the descriptor lives here once
 * instead of as two identical structs with a cast at every call.
 *
 * Freestanding on purpose -- no Zephyr, no LVGL: pp_gfx.c is also linked
 * into the host shape preview (firmware/ui/hostpreview/) with a plain host
 * compiler, which is where pp_gfx.c's zero-dependency rule comes from.
 * ui_gfx_internal.h is not part of the ABI (that is abi/include/aster/gfx.h);
 * it is private to firmware/ui.
 */
#ifndef ASTER_UI_GFX_INTERNAL_H
#define ASTER_UI_GFX_INTERNAL_H

#include <stdint.h>

/* RGB565 target: row-major, `pitch` in PIXELS (not bytes). */
typedef struct {
	uint16_t *px;
	uint32_t w, h;
	uint32_t pitch;		/* in PIXELS */
} ppg_target_t;

#endif /* ASTER_UI_GFX_INTERNAL_H */
