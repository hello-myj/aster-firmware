/* SPDX-License-Identifier: MIT */
#pragma once
#include <wasm_export.h>
#include <aster/event.h>
NativeSymbol *aster_idf_timer_symbols(uint32_t *count);
bool aster_idf_next_timer(aster_event_t *event);
void aster_idf_timers_cleanup(void);
