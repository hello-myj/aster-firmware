/* SPDX-License-Identifier: MIT */
#ifndef ASTER_TOUCH_UI_H
#define ASTER_TOUCH_UI_H

#include <stdbool.h>
#include <stdint.h>

#define ASTER_TOUCH_UI_NO_SLOT 0xffu

struct aster_touch_ui_state {
	uint8_t primary_slot;
	bool suppress_until_clear;
	bool pointer_down;
	uint8_t batch_suppress_start;
};

struct aster_touch_ui_decision {
	bool route;
	bool release_pointer;
	bool reset_gesture;
};

void aster_touch_ui_reset(struct aster_touch_ui_state *state);
/* Pre-scan a drained FIFO batch so the first DOWN of a queued two-finger
 * interaction cannot reach single-pointer UI before its second DOWN is read. */
void aster_touch_ui_begin_batch(struct aster_touch_ui_state *state,
				const uint32_t *contact_masks, uint8_t count);
struct aster_touch_ui_decision aster_touch_ui_accept(
	struct aster_touch_ui_state *state, uint8_t index, uint32_t action,
	uint32_t slot, uint32_t contact_mask);

#endif
