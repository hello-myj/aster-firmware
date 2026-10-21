/* SPDX-License-Identifier: MIT */
#ifndef ASTER_TOUCH_STATE_H
#define ASTER_TOUCH_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ASTER_TOUCH_SLOT_COUNT 2u
#define ASTER_TOUCH_EVENT_QUEUE 16u
#define ASTER_TOUCH_HW_ID_MAX 15u
#define ASTER_TOUCH_HW_ID_SYNTH0 0xfeu
#define ASTER_TOUCH_HW_ID_SYNTH1 0xffu

enum aster_touch_sample_state {
	ASTER_TOUCH_SAMPLE_NONE = 0,
	ASTER_TOUCH_SAMPLE_ACTIVE = 1,
	ASTER_TOUCH_SAMPLE_RELEASE = 2,
};

struct aster_touch_sample {
	uint16_t x;
	uint16_t y;
	uint8_t id;
	uint8_t state;
	/* Explicit controller DOWN (event 0), distinct from held CONTACT. */
	bool fresh_down;
};

struct aster_touch_frame {
	uint8_t count;
	struct aster_touch_sample points[ASTER_TOUCH_SLOT_COUNT];
};

struct aster_touch_event {
	uint32_t x;
	uint32_t y;
	uint32_t action;
	uint32_t slot;
	/* Physical/synthetic contacts that were held when this event was read
	 * from its controller frame. Used to quarantine multi-contact UI gestures
	 * even when the consumer drains a delayed event batch. */
	uint32_t contact_mask;
};

struct aster_touch_slot_state {
	uint16_t x;
	uint16_t y;
	uint8_t id;
	bool down;
};

struct aster_touch_state {
	struct aster_touch_slot_state slots[ASTER_TOUCH_SLOT_COUNT];
	struct aster_touch_event events[ASTER_TOUCH_EVENT_QUEUE];
	uint8_t event_head;
	uint8_t event_count;
	uint8_t io_errors;
	uint64_t error_since_ms;
	bool have_error_time;
	bool ignore_until_clear;
};

void aster_touch_state_init(struct aster_touch_state *state);
/* A valid complete controller frame. Malformed frames return -1 and leave
 * the live state untouched. */
int aster_touch_state_report_frame(struct aster_touch_state *state,
				   const struct aster_touch_frame *frame);
/* Keep a brief bus fault from looking like release; five consecutive 20 ms
 * failures or 120 ms continuous failure force a release and require a clean
 * all-up frame or an explicit fresh-DOWN epoch before accepting contacts. */
void aster_touch_state_report_error(struct aster_touch_state *state,
				    uint64_t now_ms);
int aster_touch_state_poll(struct aster_touch_state *state,
			   struct aster_touch_event *events, int max);
void aster_touch_state_snapshot(const struct aster_touch_state *state,
				uint32_t *points, uint32_t max);
void aster_touch_state_inject(struct aster_touch_state *state, uint32_t slot,
			      uint32_t x, uint32_t y, uint32_t action);
void aster_touch_state_clear(struct aster_touch_state *state);

#endif
