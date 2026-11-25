/* SPDX-License-Identifier: MIT */
#pragma once
#include <lvgl.h>

lv_obj_t *ai_avatar_create(lv_obj_t *parent);
void ai_avatar_play(lv_obj_t *image, const char *label, bool animate);
