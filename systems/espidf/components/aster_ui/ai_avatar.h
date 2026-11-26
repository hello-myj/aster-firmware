/* SPDX-License-Identifier: MIT */
#pragma once
#include "avatar_asset.h"
#include <lvgl.h>

lv_obj_t *ai_avatar_create(lv_obj_t *parent, bool upper_body);
struct ai_avatar_catalog ai_avatar_catalog(lv_obj_t *image);
void ai_avatar_perform(lv_obj_t *image, const struct ai_avatar_cue *cue);
void ai_avatar_set_upper_body(lv_obj_t *image, bool upper_body);
void ai_avatar_set_zoom(lv_obj_t *image, int percent);
int ai_avatar_max_zoom(lv_obj_t *image);
#ifdef ASTER_AI_RIG_LIVE
void ai_avatar_set_autonomy(lv_obj_t *image, bool gestures, bool idle);
#endif
