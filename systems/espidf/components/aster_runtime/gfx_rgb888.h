/* SPDX-License-Identifier: MIT */
#pragma once
#include <aster/gfx_rgb888.h>
#include <aster/error.h>
int aster_idf_gfx888(uint8_t *pixels,uint32_t width,uint32_t height,bool rgb,
 const aster_gfx_rgb888_cmd_t *commands,uint32_t count,const uint8_t *arena,uint32_t arena_len);
