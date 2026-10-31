/* SPDX-License-Identifier: MIT */
#pragma once
#include <aster/aster.h>
#include <wasm_export.h>
#include "aster_idf_runtime.h"
NativeSymbol *aster_idf_graphics_symbols(uint32_t *count);
void aster_idf_graphics_cleanup(wasm_module_inst_t instance);
bool aster_idf_touch_event(aster_event_t *event);
void aster_idf_map_touch(aster_event_t *event);
void aster_idf_graphics_reset_touch(void);
int aster_idf_feature(uint32_t bit);
int aster_idf_display_count(void);
