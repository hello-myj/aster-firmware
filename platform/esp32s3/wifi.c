/* SPDX-License-Identifier: MIT
 * ESP32-S3 station Wi-Fi for `hw wifi` (scan / join / status).
 */
#include "aster_platform.h"

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(aster_wifi, LOG_LEVEL_INF);

#define SCAN_TO_MS   15000
#define CONN_TO_MS   35000
#define DHCP_TO_MS   15000

#define WIFI_EVT_SCAN   (NET_EVENT_WIFI_SCAN_RESULT | \
			 NET_EVENT_WIFI_SCAN_DONE | \
			 NET_EVENT_WIFI_CONNECT_RESULT | \
			 NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_if *iface;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static K_MUTEX_DEFINE(wifi_mu);
static K_SEM_DEFINE(scan_done, 0, 1);
static K_SEM_DEFINE(conn_done, 0, 1);
static K_SEM_DEFINE(dhcp_done, 0, 1);

static aster_wifi_ap_t scan_aps[ASTER_WIFI_AP_MAX];
static int scan_n;
static int conn_status = -1;
static char req_ssid[ASTER_WIFI_SSID_MAX + 1];
static char req_psk[65];
/* Kept alongside the buffer because the length is what says "open AP"
 * (0) vs "secured" (8..64): aster_plat_wifi_reconnect() has to hand the
 * same answer back to aster_plat_wifi_connect() that it was given. */
static size_t req_psk_len;

static uint8_t sta_security(uint8_t sec)
{
	switch (sec) {
	case WIFI_SECURITY_TYPE_NONE:
		return WIFI_SECURITY_TYPE_NONE;
	default:
		/* Mixed WPA/WPA2 scan results often arrive as UNKNOWN;
		 * ESP32 then rejects connect. PSK is the working STA mode. */
		return WIFI_SECURITY_TYPE_PSK;
	}
}

static int scan_find(const char *ssid)
{
	int i;

	for (i = 0; i < scan_n; i++) {
		if (strcmp(scan_aps[i].ssid, ssid) == 0) {
			return i;
		}
	}
	return -1;
}

static void scan_add(const struct wifi_scan_result *entry)
{
	char ssid[ASTER_WIFI_SSID_MAX + 1];
	size_t n;
	int idx;

	if (!entry || entry->ssid_length == 0) {
		return;
	}
	n = entry->ssid_length;
	if (n > ASTER_WIFI_SSID_MAX) {
		n = ASTER_WIFI_SSID_MAX;
	}
	memcpy(ssid, entry->ssid, n);
	ssid[n] = 0;

	idx = scan_find(ssid);
	if (idx >= 0) {
		if (entry->rssi > scan_aps[idx].rssi) {
			scan_aps[idx].rssi = entry->rssi;
			scan_aps[idx].channel = entry->channel;
			scan_aps[idx].security = (uint8_t)entry->security;
		}
		return;
	}
	if (scan_n >= ASTER_WIFI_AP_MAX) {
		return;
	}
	memcpy(scan_aps[scan_n].ssid, ssid, n + 1);
	scan_aps[scan_n].rssi = entry->rssi;
	scan_aps[scan_n].channel = entry->channel;
	scan_aps[scan_n].security = (uint8_t)entry->security;
	scan_n++;
}

static void fill_ip(char *dst, size_t max)
{
	struct net_in_addr *addr;
	char buf[NET_IPV4_ADDR_LEN];

	dst[0] = 0;
	if (!iface) {
		return;
	}
	addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (!addr) {
		return;
	}
	net_addr_ntop(AF_INET, addr, buf, sizeof(buf));
	strncpy(dst, buf, max - 1);
	dst[max - 1] = 0;
}

static void wifi_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			 struct net_if *ift)
{
	ARG_UNUSED(ift);

	switch (event) {
	case NET_EVENT_WIFI_SCAN_RESULT:
		scan_add((const struct wifi_scan_result *)cb->info);
		break;
	case NET_EVENT_WIFI_SCAN_DONE:
		k_sem_give(&scan_done);
		break;
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *st = cb->info;

		/* With WIFI_STA_AUTO_DHCPV4 the ESP32 driver raises this
		 * only after DHCP binds, so an IP should already be there. */
		conn_status = st ? st->status : -EIO;
		k_sem_give(&conn_done);
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		conn_status = -ENOTCONN;
		k_sem_give(&conn_done);
		break;
	default:
		break;
	}
}

static void ipv4_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			 struct net_if *ift)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(ift);

	if (event == NET_EVENT_IPV4_DHCP_BOUND) {
		k_sem_give(&dhcp_done);
	}
}

static void scan_sort(void)
{
	int i, j;

	for (i = 0; i < scan_n; i++) {
		for (j = i + 1; j < scan_n; j++) {
			if (scan_aps[j].rssi > scan_aps[i].rssi) {
				aster_wifi_ap_t tmp = scan_aps[i];

				scan_aps[i] = scan_aps[j];
				scan_aps[j] = tmp;
			}
		}
	}
}

static int wifi_ready(void)
{
	int err;

	if (!iface) {
		iface = net_if_get_wifi_sta();
		if (!iface) {
			iface = net_if_get_first_wifi();
		}
	}
	if (!iface) {
		return -ENODEV;
	}
	if (!net_if_is_up(iface)) {
		err = net_if_up(iface);
		if (err && err != -EALREADY) {
			return err;
		}
	}
	return 0;
}

int aster_plat_wifi_scan(aster_wifi_ap_t *out, int max, int *count)
{
	int err;

	if (!out || max <= 0 || !count) {
		return -EINVAL;
	}
	err = wifi_ready();
	if (err) {
		return err;
	}
	k_mutex_lock(&wifi_mu, K_FOREVER);
	scan_n = 0;
	memset(scan_aps, 0, sizeof(scan_aps));
	while (k_sem_take(&scan_done, K_NO_WAIT) == 0) {
	}
	{
		struct wifi_scan_params params = {
			.scan_type = WIFI_SCAN_TYPE_ACTIVE,
		};

		err = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &params,
			       sizeof(params));
	}
	if (err) {
		k_mutex_unlock(&wifi_mu);
		return err;
	}
	err = k_sem_take(&scan_done, K_MSEC(SCAN_TO_MS));
	if (err) {
		k_mutex_unlock(&wifi_mu);
		return -ETIMEDOUT;
	}
	scan_sort();
	if (scan_n < max) {
		max = scan_n;
	}
	memcpy(out, scan_aps, (size_t)max * sizeof(aster_wifi_ap_t));
	*count = max;
	k_mutex_unlock(&wifi_mu);
	return 0;
}

int aster_plat_wifi_connect(const char *ssid, const char *psk)
{
	struct wifi_connect_req_params params = {0};
	size_t slen;
	size_t plen;
	int hit;
	int err;
	char ip[16];

	if (!ssid || !ssid[0]) {
		return -EINVAL;
	}
	err = wifi_ready();
	if (err) {
		return err;
	}

	slen = strlen(ssid);
	if (slen > ASTER_WIFI_SSID_MAX) {
		slen = ASTER_WIFI_SSID_MAX;
	}
	plen = (psk && psk[0]) ? strlen(psk) : 0;
	if (plen && (plen < 8 || plen > 64)) {
		return -EINVAL;
	}

	k_mutex_lock(&wifi_mu, K_FOREVER);
	memcpy(req_ssid, ssid, slen);
	req_ssid[slen] = 0;
	if (plen) {
		memcpy(req_psk, psk, plen);
	}
	req_psk[plen] = 0;
	req_psk_len = plen;

	params.ssid = (const uint8_t *)req_ssid;
	params.ssid_length = (uint8_t)slen;
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;
	params.mfp = WIFI_MFP_OPTIONAL;
	params.timeout = CONN_TO_MS / 1000;

	hit = scan_find(req_ssid);
	if (plen == 0) {
		params.security = WIFI_SECURITY_TYPE_NONE;
	} else if (hit >= 0) {
		params.security = sta_security(scan_aps[hit].security);
		params.psk = (const uint8_t *)req_psk;
		params.psk_length = (uint8_t)plen;
	} else {
		params.security = WIFI_SECURITY_TYPE_PSK;
		params.psk = (const uint8_t *)req_psk;
		params.psk_length = (uint8_t)plen;
	}

	while (k_sem_take(&conn_done, K_NO_WAIT) == 0) {
	}
	while (k_sem_take(&dhcp_done, K_NO_WAIT) == 0) {
	}
	conn_status = -EINPROGRESS;
	err = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
	if (err) {
		k_mutex_unlock(&wifi_mu);
		return err;
	}
	err = k_sem_take(&conn_done, K_MSEC(CONN_TO_MS));
	if (err) {
		k_mutex_unlock(&wifi_mu);
		return -ETIMEDOUT;
	}
	if (conn_status) {
		err = conn_status > 0 ? -EIO : conn_status;
		k_mutex_unlock(&wifi_mu);
		return err;
	}

	fill_ip(ip, sizeof(ip));
	if (!ip[0]) {
		net_dhcpv4_start(iface);
		(void)k_sem_take(&dhcp_done, K_MSEC(DHCP_TO_MS));
	}
	k_mutex_unlock(&wifi_mu);
	return 0;
}

int aster_plat_wifi_disconnect(void)
{
	int err;

	err = wifi_ready();
	if (err) {
		return err;
	}
	k_mutex_lock(&wifi_mu, K_FOREVER);
	err = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	k_mutex_unlock(&wifi_mu);
	return err;
}

/* Both of these exist for the quick-settings sheet, which has one button
 * and no way to type an SSID: the only join it can offer is "the network
 * this device was last told to join". The credentials never leave the
 * port, so the answer has to be assembled here rather than by the caller. */
int aster_plat_wifi_last_ssid(char *buf, size_t n)
{
	size_t len;

	if (!buf || n == 0) {
		return -EINVAL;
	}
	buf[0] = 0;
	/* K_NO_WAIT, not K_FOREVER: a connect holds wifi_mu for up to 50 s,
	 * and this is called from the thread that draws the screen. */
	if (k_mutex_lock(&wifi_mu, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	len = strlen(req_ssid);
	if (len == 0) {
		k_mutex_unlock(&wifi_mu);
		return -ENOENT;
	}
	if (len > n - 1) {
		len = n - 1;
	}
	memcpy(buf, req_ssid, len);
	buf[len] = 0;
	k_mutex_unlock(&wifi_mu);
	return 0;
}

int aster_plat_wifi_reconnect(void)
{
	char ssid[ASTER_WIFI_SSID_MAX + 1];
	char psk[65];
	bool have_psk;

	/* Locks only long enough to take a consistent copy: the actual join
	 * re-enters aster_plat_wifi_connect(), which takes the same mutex and
	 * holds it for the whole attempt. */
	if (k_mutex_lock(&wifi_mu, K_FOREVER) != 0) {
		return -EIO;
	}
	if (req_ssid[0] == 0) {
		k_mutex_unlock(&wifi_mu);
		return -ENOENT;
	}
	memcpy(ssid, req_ssid, sizeof(ssid));
	memcpy(psk, req_psk, sizeof(psk));
	have_psk = req_psk_len != 0;
	k_mutex_unlock(&wifi_mu);

	/* NULL, not "", for an open AP: connect() derives its PSK length
	 * from the string and rejects anything shorter than 8 that is not
	 * empty. */
	return aster_plat_wifi_connect(ssid, have_psk ? psk : NULL);
}

int aster_plat_wifi_status(aster_wifi_status_t *st)
{
	struct wifi_iface_status ws = {0};
	int err;

	if (!st) {
		return -EINVAL;
	}
	memset(st, 0, sizeof(*st));
	err = wifi_ready();
	if (err) {
		st->state_txt = "down";
		st->sec_txt = "-";
		return err;
	}
	err = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &ws, sizeof(ws));
	if (err) {
		st->state_txt = "unknown";
		st->sec_txt = "-";
		return err;
	}
	st->state = ws.state;
	st->rssi = (int8_t)ws.rssi;
	st->state_txt = wifi_state_txt(ws.state);
	st->sec_txt = wifi_security_txt(ws.security);
	if (ws.ssid_len) {
		size_t n = ws.ssid_len;

		if (n > ASTER_WIFI_SSID_MAX) {
			n = ASTER_WIFI_SSID_MAX;
		}
		memcpy(st->ssid, ws.ssid, n);
		st->ssid[n] = 0;
	}
	fill_ip(st->ip, sizeof(st->ip));
	return 0;
}

const char *aster_plat_wifi_sec_txt(uint8_t security)
{
	return wifi_security_txt((enum wifi_security_type)security);
}

static int aster_wifi_sys_init(void)
{
	iface = net_if_get_wifi_sta();
	if (!iface) {
		iface = net_if_get_first_wifi();
	}
	net_mgmt_init_event_callback(&wifi_cb, wifi_handler, WIFI_EVT_SCAN);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_handler,
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&ipv4_cb);
	LOG_INF("wifi iface %s", iface ? "ready" : "missing");
	return 0;
}

SYS_INIT(aster_wifi_sys_init, APPLICATION, 90);
