/* SPDX-License-Identifier: MIT
 * Wi-Fi state, in Chinese, for the firmware's own screens (the launcher's
 * status line and the quick-settings sheet).
 *
 * The platform layer hands out state_txt as the English Zephyr enum name
 * ("COMPLETED", "4WAY_HANDSHAKE", ...). That is right for the shell and
 * wrong for this UI, so the wording is derived here from the numeric state
 * instead -- once, for both screens: two copies of this switch would drift
 * the moment a state is added to one of them.
 *
 * state 0 (DISCONNECTED) is also what a FAILED aster_plat_wifi_status()
 * leaves behind, and the caller passes -1 for that: both read as "未连接",
 * which is the honest answer either way (we do not know of a link).
 */
#ifndef ASTER_UI_WIFI_H
#define ASTER_UI_WIFI_H

#include <zephyr/net/wifi.h>

static inline const char *aster_ui_wifi_word(int state)
{
	switch (state) {
	case WIFI_STATE_COMPLETED:
		return "WiFi 已连接";
	case WIFI_STATE_AUTHENTICATING:
	case WIFI_STATE_ASSOCIATING:
	case WIFI_STATE_ASSOCIATED:
	case WIFI_STATE_4WAY_HANDSHAKE:
	case WIFI_STATE_GROUP_HANDSHAKE:
		return "WiFi 连接中";
	case WIFI_STATE_SCANNING:
		return "WiFi 扫描中";
	case WIFI_STATE_INTERFACE_DISABLED:
	case WIFI_STATE_INACTIVE:
		return "WiFi 未启用";
	default:
		return "WiFi 未连接";
	}
}

#endif /* ASTER_UI_WIFI_H */
