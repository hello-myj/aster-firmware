/* SPDX-License-Identifier: MIT */
#include "touch_ui.h"

#include "touch_state.h"
#include <aster/event.h>
#include <string.h>

#define NO_BATCH_SUPPRESS 0xffu

static bool two_or_more(uint32_t mask)
{
	return (mask & (mask - 1u)) != 0;
}

void aster_touch_ui_reset(struct aster_touch_ui_state *state)
{
	if (!state) return;
	memset(state, 0, sizeof(*state));
	state->primary_slot = ASTER_TOUCH_UI_NO_SLOT;
	state->batch_suppress_start = NO_BATCH_SUPPRESS;
}

void aster_touch_ui_begin_batch(struct aster_touch_ui_state *state,
				const uint32_t *contact_masks, uint8_t count)
{
	if (!state) return;
	state->batch_suppress_start = NO_BATCH_SUPPRESS;
	if (!contact_masks || !count) return;
	for (uint8_t i = 0; i < count; ++i) {
		if (!two_or_more(contact_masks[i])) continue;
		uint8_t start = i;
		/* Walk back over this continuous contact run. A zero-mask UP before
		 * the second finger marks a genuinely separate earlier tap. */
		while (start > 0 && contact_masks[start - 1u] != 0) --start;
		state->batch_suppress_start = start;
		return;
	}
}

struct aster_touch_ui_decision aster_touch_ui_accept(
	struct aster_touch_ui_state *state, uint8_t index, uint32_t action,
	uint32_t slot, uint32_t contact_mask)
{
	struct aster_touch_ui_decision result = {0};
	if (!state) return result;
	if (index == state->batch_suppress_start) {
		state->suppress_until_clear = true;
		state->primary_slot = ASTER_TOUCH_UI_NO_SLOT;
		result.reset_gesture = true;
		if (state->pointer_down) {
			state->pointer_down = false;
			result.release_pointer = true;
		}
	}
	if (two_or_more(contact_mask) && !state->suppress_until_clear) {
		state->suppress_until_clear = true;
		state->primary_slot = ASTER_TOUCH_UI_NO_SLOT;
		result.reset_gesture = true;
		if (state->pointer_down) {
			state->pointer_down = false;
			result.release_pointer = true;
		}
	}
	if (state->suppress_until_clear) {
		if (contact_mask == 0) state->suppress_until_clear = false;
		return result;
	}
	if (slot >= 2) return result;
	if (state->primary_slot == ASTER_TOUCH_UI_NO_SLOT) {
		if (action != ASTER_TOUCH_DOWN || !(contact_mask & (1u << slot))) return result;
		state->primary_slot = (uint8_t)slot;
	} else if (state->primary_slot != slot) {
		return result;
	}
	result.route = true;
	if (action == ASTER_TOUCH_UP) {
		state->primary_slot = ASTER_TOUCH_UI_NO_SLOT;
		state->pointer_down = false;
		/* Keep UI ownership closed until every finger is lifted. Never hand
		 * the pointer to the remaining finger midway through a gesture. */
		state->suppress_until_clear = contact_mask != 0;
	} else {
		state->pointer_down = true;
	}
	return result;
}
