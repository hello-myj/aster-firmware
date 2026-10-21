/* SPDX-License-Identifier: MIT */
#ifndef ASTER_INPUT_H
#define ASTER_INPUT_H

#include "types.h"

#define ASTER_INPUT_TOUCH    1u
#define ASTER_INPUT_KEYS     2u

typedef struct {
	uint32_t size;    /* sizeof(aster_input_info_t) */
	uint32_t version; /* 1 */
	uint32_t kinds;   /* bitmask ASTER_INPUT_x */
	uint32_t max_points;
	uint32_t has_gesture; /* reserved, 0 in V1 */
} aster_input_info_t;

ASTER_I("aster_input_get_info") int32_t aster_input_get_info(aster_input_info_t *info, uint32_t info_size);

/* Poll last touch state without waiting for events (convenience for games).
 * Returns number of points written (0..max), or negative error. Each point is
 * exactly three u32 values (x, y, action), 12 bytes. Stable slot identity is
 * available on ASTER_EVENT_TOUCH, not in this legacy snapshot record. */
ASTER_I("aster_input_touch") int32_t aster_input_touch(uint32_t points_offset, uint32_t max_points);

typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t action; /* ASTER_TOUCH_x */
} aster_touch_point_t;

/* --- keyboard / buttons (ABI 1.1) ----------------------------------------
 * Key codes are forwarded raw from the host (Zephyr INPUT_KEY_*,
 * following the Linux evdev numbering):
 * 1 = KEY_ESC, 2..11 = KEY_1..KEY_0, 14 = KEY_BACKSPACE, 28 = KEY_ENTER,
 * 57 = KEY_SPACE, 103/105/106/108 = up/left/right/down.
 * UI layers map codes to semantics; ASTER never renumbers them.
 *
 * ASTER_EVENT_KEY carries the edge stream: u.key.code / u.key.pressed (1/0).
 * aster_input_keys_state() polls instantaneous state (writes up to `max`
 * currently-pressed codes into a u32 array, returns the count written, or a
 * negative error): lets a UI re-sync held keys after missing an edge, and
 * drive auto-repeat with a timer. Codes >= 0xFFFF are not reported. */
ASTER_I("aster_input_keys_state") int32_t aster_input_keys_state(uint32_t codes_offset,
							   uint32_t max_codes);

#endif /* ASTER_INPUT_H */
