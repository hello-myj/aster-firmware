/* SPDX-License-Identifier: MIT - headless LVGL preview clock only */
#pragma once
#include <stdint.h>
extern uint32_t preview_ms;
static inline uint32_t k_uptime_get_32(void){return preview_ms;}
