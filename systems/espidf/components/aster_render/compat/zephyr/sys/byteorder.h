/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
static inline uint16_t sys_cpu_to_be16(uint16_t n) { return __builtin_bswap16(n); }
