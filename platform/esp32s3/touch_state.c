/* SPDX-License-Identifier: MIT */
#include "touch_state.h"

#include <errno.h>
#include <string.h>

#define ASTER_TOUCH_IO_ERROR_LIMIT 5u
#define ASTER_TOUCH_IO_ERROR_MS 120u
#define ASTER_TOUCH_ACTION_DOWN 1u
#define ASTER_TOUCH_ACTION_MOVE 2u
#define ASTER_TOUCH_ACTION_UP 3u
#define ASTER_TOUCH_TERMINATOR 0xffffffffu

static uint32_t live_mask(const struct aster_touch_state *state)
{
	uint32_t mask = 0;
	for (uint32_t i = 0; i < ASTER_TOUCH_SLOT_COUNT; i++) {
		if (state->slots[i].down) mask |= 1u << i;
	}
	return mask;
}

static int slot_for_id(const struct aster_touch_state *state, uint8_t id)
{
	for (int i = 0; i < (int)ASTER_TOUCH_SLOT_COUNT; i++) {
		if (state->slots[i].down && state->slots[i].id == id) return i;
	}
	return -1;
}

static int free_slot(const struct aster_touch_state *state)
{
	for (int i = 0; i < (int)ASTER_TOUCH_SLOT_COUNT; i++) {
		if (!state->slots[i].down) return i;
	}
	return -1;
}

static void force_recovery(struct aster_touch_state *state)
{
	/* A finite queue cannot retain an unbounded sequence of taps. Flush it,
	 * release both possible guest slots, then ignore new contacts until a
	 * complete all-up frame or explicit fresh DOWN establishes a clean epoch. */
	state->event_head = 0;
	state->event_count = 0;
	for (uint32_t slot = 0; slot < ASTER_TOUCH_SLOT_COUNT; slot++) {
		struct aster_touch_slot_state *pt = &state->slots[slot];
		state->events[state->event_count++] = (struct aster_touch_event) {
			.x = pt->x, .y = pt->y, .action = ASTER_TOUCH_ACTION_UP,
			.slot = slot, .contact_mask = 0,
		};
		pt->down = false;
	}
	state->ignore_until_clear = true;
}

static bool enqueue(struct aster_touch_state *state,
		    const struct aster_touch_event *event)
{
	/* MOVE has no edge semantics. Replace the most recent pending MOVE for
	 * this slot when no later edge for that slot follows it. This preserves
	 * per-slot order and keeps a slow renderer from filling the ring. */
	if (event->action == ASTER_TOUCH_ACTION_MOVE) {
		for (int i = (int)state->event_count - 1; i >= 0; i--) {
			uint32_t at = (state->event_head + (uint32_t)i) % ASTER_TOUCH_EVENT_QUEUE;
			struct aster_touch_event *old = &state->events[at];
			if (old->slot == event->slot) {
				if (old->action == ASTER_TOUCH_ACTION_MOVE) {
					*old = *event;
					return true;
				}
				break;
			}
		}
	}
	if (state->event_count >= ASTER_TOUCH_EVENT_QUEUE) {
		force_recovery(state);
		return false;
	}
	uint32_t at = (state->event_head + state->event_count) % ASTER_TOUCH_EVENT_QUEUE;
	state->events[at] = *event;
	state->event_count++;
	return true;
}

static void stage_event(struct aster_touch_event *pending, uint8_t *count,
			uint32_t slot, uint32_t x, uint32_t y, uint32_t action)
{
	if (*count < ASTER_TOUCH_SLOT_COUNT * 2u) {
		pending[*count] = (struct aster_touch_event) {
			.x = x, .y = y, .action = action, .slot = slot,
		};
		(*count)++;
	}
}

void aster_touch_state_init(struct aster_touch_state *state)
{
	if (state) memset(state, 0, sizeof(*state));
}

void aster_touch_state_clear(struct aster_touch_state *state)
{
	if (state) {
		memset(state, 0, sizeof(*state));
	}
}

int aster_touch_state_report_frame(struct aster_touch_state *state,
				   const struct aster_touch_frame *frame)
{
	if (!state || !frame || frame->count > ASTER_TOUCH_SLOT_COUNT) return -EINVAL;

	uint16_t seen_ids = 0;
	for (uint32_t i = 0; i < frame->count; i++) {
		const struct aster_touch_sample *pt = &frame->points[i];
		if (pt->id > ASTER_TOUCH_HW_ID_MAX ||
		    pt->state > ASTER_TOUCH_SAMPLE_RELEASE ||
		    (seen_ids & (1u << pt->id))) return -EINVAL;
		seen_ids |= (uint16_t)(1u << pt->id);
	}
	state->io_errors = 0;
	state->have_error_time = false;
	if (state->ignore_until_clear) {
		/* A controller may keep the final explicit UP record rather than
		 * publishing count=0. A valid frame containing only releases also
		 * establishes all-up; malformed or unknown-state frames do not. */
		bool all_released = true;
		bool new_epoch = frame->count != 0;
		bool have_down = false;
		for (uint32_t i = 0; i < frame->count; i++) {
			if (frame->points[i].state != ASTER_TOUCH_SAMPLE_RELEASE)
				all_released = false;
			if (frame->points[i].state == ASTER_TOUCH_SAMPLE_ACTIVE &&
			    frame->points[i].fresh_down) have_down = true;
			else if (frame->points[i].state != ASTER_TOUCH_SAMPLE_RELEASE)
				new_epoch = false;
		}
		if (all_released) {
			state->ignore_until_clear = false;
			return 0;
		}
		/* If the final UP was lost, waiting for count=0 can be permanent
		 * on a controller that returns unreadable idle data. Only an explicit
		 * new DOWN epoch may reopen input; old held CONTACT never does. */
		if (!new_epoch || !have_down) return 0;
		state->ignore_until_clear = false;
	}

	struct aster_touch_event pending[ASTER_TOUCH_SLOT_COUNT * 2u];
	uint8_t pending_count = 0;

	/* Explicit releases happen before new IDs are allocated. A reused hardware
	 * ID therefore cannot inherit its former logical contact's lifecycle. */
	for (uint32_t i = 0; i < frame->count; i++) {
		const struct aster_touch_sample *pt = &frame->points[i];
		if (pt->state != ASTER_TOUCH_SAMPLE_RELEASE) continue;
		int slot = slot_for_id(state, pt->id);
		if (slot >= 0) {
			struct aster_touch_slot_state *old = &state->slots[slot];
			stage_event(pending, &pending_count, (uint32_t)slot,
				    old->x, old->y, ASTER_TOUCH_ACTION_UP);
			old->down = false;
		}
	}

	/* A full valid report is also authoritative for vanished IDs. */
	for (uint32_t slot = 0; slot < ASTER_TOUCH_SLOT_COUNT; slot++) {
		struct aster_touch_slot_state *old = &state->slots[slot];
		/* Synthetic shell/debug contacts own their slot until injection sends
		 * UP. Periodic empty hardware reports must not cancel a software swipe. */
		if (old->down && old->id <= ASTER_TOUCH_HW_ID_MAX &&
		    !(seen_ids & (1u << old->id))) {
			stage_event(pending, &pending_count, slot, old->x, old->y,
				    ASTER_TOUCH_ACTION_UP);
			old->down = false;
		}
	}

	for (uint32_t i = 0; i < frame->count; i++) {
		const struct aster_touch_sample *pt = &frame->points[i];
		if (pt->state == ASTER_TOUCH_SAMPLE_RELEASE) continue;
		int slot = slot_for_id(state, pt->id);
		if (pt->state == ASTER_TOUCH_SAMPLE_NONE) {
			/* Some firmware versions emit event=3 for a still-live point.
			 * Keep its old slot if it was known, but never create a DOWN from
			 * an unconfirmed record. */
			continue;
		}
		if (slot < 0) slot = free_slot(state);
		if (slot < 0) {
			force_recovery(state);
			return -ENOSPC;
		}
		struct aster_touch_slot_state *dst = &state->slots[slot];
		uint32_t action;
		if (!dst->down) {
			action = ASTER_TOUCH_ACTION_DOWN;
		} else if (dst->x == pt->x && dst->y == pt->y) {
			continue;
		} else {
			action = ASTER_TOUCH_ACTION_MOVE;
		}
		dst->id = pt->id;
		dst->x = pt->x;
		dst->y = pt->y;
		dst->down = true;
		stage_event(pending, &pending_count, (uint32_t)slot,
			    pt->x, pt->y, action);
	}

	uint32_t mask = live_mask(state);
	for (uint32_t i = 0; i < pending_count; i++) {
		pending[i].contact_mask = mask;
		if (!enqueue(state, &pending[i])) return -ENOSPC;
	}
	return 0;
}

void aster_touch_state_report_error(struct aster_touch_state *state,
				    uint64_t now_ms)
{
	if (!state) return;
	/* Software-injected contacts are not observations from this I2C read.
	 * An unrelated hardware read failure must not release a held CLI slot. */
	bool hardware_down = false;
	for (uint32_t i = 0; i < ASTER_TOUCH_SLOT_COUNT; i++) {
		if (state->slots[i].down && state->slots[i].id <= ASTER_TOUCH_HW_ID_MAX)
			hardware_down = true;
	}
	if (state->io_errors < UINT8_MAX) state->io_errors++;
	if (!state->have_error_time) {
		state->error_since_ms = now_ms;
		state->have_error_time = true;
	}
	if (!state->ignore_until_clear && hardware_down &&
	    (state->io_errors >= ASTER_TOUCH_IO_ERROR_LIMIT ||
	     now_ms - state->error_since_ms >= ASTER_TOUCH_IO_ERROR_MS)) {
		force_recovery(state);
	}
}

int aster_touch_state_poll(struct aster_touch_state *state,
			   struct aster_touch_event *events, int max)
{
	if (!state || max <= 0) return 0;
	if (!events) return -EINVAL;
	int n = 0;
	while (state->event_count && n < max) {
		events[n++] = state->events[state->event_head];
		state->event_head = (state->event_head + 1u) % ASTER_TOUCH_EVENT_QUEUE;
		state->event_count--;
	}
	return n;
}

void aster_touch_state_snapshot(const struct aster_touch_state *state,
				uint32_t *points, uint32_t max)
{
	if (!points || max == 0) return;
	if (max > ASTER_TOUCH_SLOT_COUNT) max = ASTER_TOUCH_SLOT_COUNT;
	uint32_t n = 0;
	if (state) {
		for (uint32_t slot = 0; slot < ASTER_TOUCH_SLOT_COUNT && n < max; slot++) {
			const struct aster_touch_slot_state *pt = &state->slots[slot];
			if (!pt->down) continue;
			points[n * 4u + 0] = pt->x;
			points[n * 4u + 1] = pt->y;
			points[n * 4u + 2] = ASTER_TOUCH_ACTION_DOWN;
			points[n * 4u + 3] = slot;
			n++;
		}
	}
	if (n < max) points[n * 4u + 2] = ASTER_TOUCH_TERMINATOR;
}

void aster_touch_state_inject(struct aster_touch_state *state, uint32_t slot,
			      uint32_t x, uint32_t y, uint32_t action)
{
	if (!state || slot >= ASTER_TOUCH_SLOT_COUNT ||
	    action < ASTER_TOUCH_ACTION_DOWN || action > ASTER_TOUCH_ACTION_UP) return;
	struct aster_touch_slot_state *pt = &state->slots[slot];
	uint32_t actual = action;
	if (action == ASTER_TOUCH_ACTION_DOWN) {
		if (pt->down) actual = ASTER_TOUCH_ACTION_MOVE;
		pt->down = true;
		pt->id = slot == 0 ? ASTER_TOUCH_HW_ID_SYNTH0 : ASTER_TOUCH_HW_ID_SYNTH1;
	} else if (action == ASTER_TOUCH_ACTION_MOVE && !pt->down) {
		return;
	} else if (action == ASTER_TOUCH_ACTION_UP && !pt->down) {
		return;
	}
	pt->x = (uint16_t)x;
	pt->y = (uint16_t)y;
	if (actual == ASTER_TOUCH_ACTION_UP) pt->down = false;
	struct aster_touch_event event = {
		.x = x, .y = y, .action = actual, .slot = slot,
		.contact_mask = live_mask(state),
	};
	(void)enqueue(state, &event);
}
