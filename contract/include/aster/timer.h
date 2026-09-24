/* SPDX-License-Identifier: MIT */
#ifndef ASTER_TIMER_H
#define ASTER_TIMER_H

#include "types.h"

/* One-shot or repeating timer; fires ASTER_EVENT_TIMER on the app event loop. */
ASTER_I("aster_timer_create") int32_t aster_timer_create(uint32_t period_ms, uint32_t repeat,
						   uint32_t user, aster_handle_t *out);
ASTER_I("aster_timer_destroy") int32_t aster_timer_destroy(aster_handle_t timer);

#endif /* ASTER_TIMER_H */
