/* SPDX-License-Identifier: MIT */
#ifndef ASTER_WIFI_SETTINGS_H
#define ASTER_WIFI_SETTINGS_H
#include <lvgl.h>
/* NULL close_owner deletes only the settings page; a panel owner may also
 * restore the application snapshot when closing its container. */
void aster_wifi_settings_open(lv_obj_t *parent, void (*close_owner)(void));
void aster_wifi_settings_tick(void);
void aster_wifi_settings_warm(void);
bool aster_wifi_settings_is_open(void);
#endif
