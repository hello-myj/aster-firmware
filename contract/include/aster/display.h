/* SPDX-License-Identifier: MIT */
#ifndef ASTER_DISPLAY_H
#define ASTER_DISPLAY_H

#include "types.h"

/* Orientation */
#define ASTER_ORIENT_PORTRAIT  0u
#define ASTER_ORIENT_LANDSCAPE 1u

typedef struct {
	uint32_t size;    /* sizeof(aster_display_info_t) */
	uint32_t version; /* 1 */

	uint32_t width;  /* the glass, in LOGICAL pixels */
	uint32_t height;
	uint32_t dpi_x100;            /* dpi * 100; 0 = unknown */
	uint32_t native_format;       /* aster_pixel_format_t; the fast path */
	uint64_t supported_formats;   /* bit = ASTER_FMT_BIT(fmt); native_format is
	                               * always a member. See ASTER_FMT_BIT in
	                               * types.h for the bit-collision caveat. */
	uint32_t refresh_rate;        /* Hz, 0 unknown */
	uint32_t orientation;         /* ASTER_ORIENT_x */
	uint32_t flags;

	/* [3.2] The coordinate space surfaces are created in and touch samples
	 * are reported in. Equal to width/height on any board that draws app
	 * pixels 1:1 onto the glass; smaller on one that scales, where the
	 * guest is given a space sized to the app rather than to the panel.
	 *
	 * Lay out against these, not against width/height. A surface request is
	 * validated against them, so an app that assumes the reference board's
	 * numbers is refused on a board with a divisor -- which is the loud
	 * failure; the quiet one would be an app that renders off-screen. */
	uint32_t app_width;
	uint32_t app_height;
} aster_display_info_t;

ASTER_I("aster_display_count") int32_t aster_display_count(void);
ASTER_I("aster_display_get_info") int32_t aster_display_get_info(uint32_t index,
							   aster_display_info_t *info, uint32_t info_size);

#endif /* ASTER_DISPLAY_H */
