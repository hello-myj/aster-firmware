/* SPDX-License-Identifier: MIT */
#ifndef ASTER_WIFI_SETTINGS_H
#define ASTER_WIFI_SETTINGS_H
#include <lvgl.h>
void aster_wifi_settings_open(lv_obj_t *parent);
void aster_wifi_settings_tick(void);
void aster_wifi_settings_warm(void);
bool aster_wifi_settings_is_open(void);
#endif
