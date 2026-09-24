/* SPDX-License-Identifier: MIT */
#ifndef ASTER_EVENT_H
#define ASTER_EVENT_H

#include "types.h"

/* Event types (design doc sec.58) */
#define ASTER_EVENT_FRAME     1u /* periodic render tick */
#define ASTER_EVENT_TOUCH     2u /* down/move/up, see action */
#define ASTER_EVENT_TIMER     3u
#define ASTER_EVENT_KEY       4u
#define ASTER_EVENT_SYS       5u /* system notification (memory pressure...) */
/* ASTER_EVENT_UI (6) retired with ABI 3.0: the wasm-side widget runtime is
 * gone; drawing apps render through the gfx command engine (ABI 2.2). */

/* Touch actions */
#define ASTER_TOUCH_DOWN 1u
#define ASTER_TOUCH_MOVE 2u
#define ASTER_TOUCH_UP   3u

/* KEY codes: raw input codes forwarded from the host (INPUT_KEY_*) */

/* SYS notifications */
#define ASTER_SYS_MEM_PRESSURE 1u
#define ASTER_SYS_ORIENT       2u

typedef struct {
	uint32_t size;    /* sizeof(aster_event_t) */
	uint32_t version; /* 1 */
	uint32_t type;
	uint32_t flags;
	uint64_t timestamp_ms;
	union {
		struct {
			uint32_t x;
			uint32_t y;
			uint32_t action; /* ASTER_TOUCH_x */
			uint32_t slot;
		} touch;
		struct {
			uint32_t id;    /* timer handle passed back */
			uint32_t user;  /* opaque value set at create */
		} timer;
		struct {
			uint32_t code;
			uint32_t pressed;
		} key;
		struct {
			uint32_t code;  /* ASTER_SYS_x */
			uint32_t value;
		} sys;
		struct {
			uint32_t frame; /* frames since app start */
		} frame;
		uint32_t data[8];
	} u;
} aster_event_t;

#endif /* ASTER_EVENT_H */
