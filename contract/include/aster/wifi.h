/* SPDX-License-Identifier: MIT
 * ASTER ABI 3.5 - asynchronous Wi-Fi station control.
 *
 * Gated by the manifest permission "wifi" and the ASTER_FEATURE_WIFI
 * capability. Scan and connection work runs outside the APP thread. Poll
 * aster_wifi_status() for operation completion and aster_wifi_scan_results()
 * for scan results. A successful connection is saved by the device and is
 * retried after boot when storage is available.
 */
#ifndef ASTER_WIFI_H
#define ASTER_WIFI_H

#include "types.h"

#define ASTER_WIFI_SSID_MAX 32
#define ASTER_WIFI_AP_MAX   16
#define ASTER_WIFI_IP_MAX   15

typedef enum {
	ASTER_WIFI_STATE_DISCONNECTED = 0,
	ASTER_WIFI_STATE_DISABLED,
	ASTER_WIFI_STATE_INACTIVE,
	ASTER_WIFI_STATE_SCANNING,
	ASTER_WIFI_STATE_AUTHENTICATING,
	ASTER_WIFI_STATE_ASSOCIATING,
	ASTER_WIFI_STATE_ASSOCIATED,
	ASTER_WIFI_STATE_FOUR_WAY_HANDSHAKE,
	ASTER_WIFI_STATE_GROUP_HANDSHAKE,
	ASTER_WIFI_STATE_COMPLETED,
	ASTER_WIFI_STATE_UNKNOWN,
} aster_wifi_state_t;

typedef enum {
	ASTER_WIFI_OP_NONE = 0,
	ASTER_WIFI_OP_SCAN,
	ASTER_WIFI_OP_CONNECT,
	ASTER_WIFI_OP_DISCONNECT,
	ASTER_WIFI_OP_RECONNECT,
	ASTER_WIFI_OP_FORGET,
} aster_wifi_operation_t;

typedef enum {
	ASTER_WIFI_SECURITY_OPEN = 0,
	ASTER_WIFI_SECURITY_PERSONAL,
	ASTER_WIFI_SECURITY_ENTERPRISE,
	ASTER_WIFI_SECURITY_WEP,
	ASTER_WIFI_SECURITY_OTHER = 255,
} aster_wifi_security_t;

typedef struct {
	uint32_t size;    /* sizeof(aster_wifi_ap_info_t) */
	uint32_t version; /* 1 */
	char ssid[ASTER_WIFI_SSID_MAX + 1];
	int32_t rssi;     /* dBm */
	uint32_t channel;
	uint32_t security; /* aster_wifi_security_t */
} aster_wifi_ap_info_t;

typedef struct {
	uint32_t size;    /* set to sizeof(aster_wifi_info_t) */
	uint32_t version; /* 1 */
	uint32_t state;   /* aster_wifi_state_t */
	uint32_t operation; /* aster_wifi_operation_t currently running */
	uint32_t last_operation;
	int32_t last_result; /* ASTER_OK / ASTER_E... from the last operation */
	int32_t rssi;        /* dBm; meaningful while associated */
	uint32_t security;   /* aster_wifi_security_t */
	char ssid[ASTER_WIFI_SSID_MAX + 1];
	char ipv4[ASTER_WIFI_IP_MAX + 1];
} aster_wifi_info_t;

/* Queue one scan. Returns ASTER_EBUSY if another Wi-Fi operation is active. */
ASTER_I("aster_wifi_scan_start") int32_t aster_wifi_scan_start(void);

/* Returns AP count on success, ASTER_EAGAIN while scanning, ASTER_ENOENT
 * before the first completed scan. Results are sorted by RSSI, strongest first. */
ASTER_I("aster_wifi_scan_results") int32_t aster_wifi_scan_results(
	aster_wifi_ap_info_t *out, uint32_t capacity);

/* Queue a 2.4 GHz personal/open-network connection. An empty password selects
 * an open AP. Credentials are saved only after a successful connection. */
ASTER_I("aster_wifi_connect") int32_t aster_wifi_connect(
	const char *ssid, uint32_t ssid_len,
	const char *password, uint32_t password_len);
ASTER_I("aster_wifi_disconnect") int32_t aster_wifi_disconnect(void);
ASTER_I("aster_wifi_reconnect") int32_t aster_wifi_reconnect(void);
ASTER_I("aster_wifi_forget") int32_t aster_wifi_forget(void);

/* Fill current link and operation state. `size` must be at least the v1
 * prefix through `last_result`; larger caller buffers are supported. */
ASTER_I("aster_wifi_status") int32_t aster_wifi_status(
	aster_wifi_info_t *status, uint32_t size);

/* Copy the saved SSID including NUL. Returns ASTER_ENOENT if none is saved. */
ASTER_I("aster_wifi_get_saved_ssid") int32_t aster_wifi_get_saved_ssid(
	char *out, uint32_t capacity);

#endif /* ASTER_WIFI_H */
