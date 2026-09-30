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
#if defined(CONFIG_NET_DHCPV4_SERVER)
#include <zephyr/net/dhcpv4_server.h>
#endif
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>

#include "aster_kv.h"

LOG_MODULE_REGISTER(aster_wifi, LOG_LEVEL_INF);

#define SCAN_TO_MS   15000
#define CONN_TO_MS   35000
#define DHCP_TO_MS   15000
#define DISC_TO_MS   5000
#define WIFI_KV_ID   "aster_wifi"
#define WIFI_KV_KEY  "profile"
#define WIFI_PROFILE_VERSION 1u

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
static atomic_t conn_wait_kind;
enum { CONN_WAIT_NONE, CONN_WAIT_CONNECT, CONN_WAIT_DISCONNECT };
static char req_ssid[ASTER_WIFI_SSID_MAX + 1];
static char req_psk[65];
/* Kept alongside the buffer because the length is what says "open AP"
 * (0) vs "secured" (8..64): aster_plat_wifi_reconnect() has to hand the
 * same answer back to aster_plat_wifi_connect() that it was given. */
static size_t req_psk_len;

typedef struct {
	uint32_t version;
	char ssid[ASTER_WIFI_SSID_MAX + 1];
	char psk[65];
	uint32_t psk_len;
} wifi_profile_t;

static K_MUTEX_DEFINE(job_mu);
/* Connecting persists the profile through KV -> FatFS -> SDHC. That call
 * chain exceeds 3 KB and used to overwrite the adjacent sleeping audio
 * thread's saved context. Reserve the same stack budget as the main thread
 * so filesystem calls and their interrupt/context-save frames fit. */
static K_THREAD_STACK_DEFINE(job_stack, 8192);
static struct k_work_q job_q;
static bool job_q_up;
static bool job_busy;
static uint32_t job_op;
static uint32_t job_last_op;
static int job_last_result = -ENOENT;
static aster_wifi_ap_t job_scan[ASTER_WIFI_AP_MAX];
static int job_scan_n;
static bool job_scan_valid;
static char job_ssid[ASTER_WIFI_SSID_MAX + 1];
static char job_psk[65];
static size_t job_psk_len;
static wifi_profile_t hotspot_profile = {.version = WIFI_PROFILE_VERSION, .ssid = "ASTER"};
static bool hotspot_enabled;
static bool hotspot_configured;

static int queue_wifi_job(uint32_t op, const char *ssid, const char *psk);
static void wifi_job_fn(struct k_work *work);
K_WORK_DEFINE(wifi_job, wifi_job_fn);

static int sta_security(uint8_t sec, enum wifi_security_type *out)
{
	switch (sec) {
	case WIFI_SECURITY_TYPE_NONE:
		*out = WIFI_SECURITY_TYPE_NONE;
		return 0;
	case WIFI_SECURITY_TYPE_SAE:
	case WIFI_SECURITY_TYPE_SAE_H2E:
	case WIFI_SECURITY_TYPE_SAE_AUTO:
	case WIFI_SECURITY_TYPE_SAE_EXT_KEY:
		*out = sec;
		return 0;
	case WIFI_SECURITY_TYPE_EAP:
	case WIFI_SECURITY_TYPE_EAP_PEAP_MSCHAPV2:
	case WIFI_SECURITY_TYPE_EAP_PEAP_GTC:
	case WIFI_SECURITY_TYPE_EAP_TTLS_MSCHAPV2:
	case WIFI_SECURITY_TYPE_EAP_PEAP_TLS:
	case WIFI_SECURITY_TYPE_FT_EAP:
	case WIFI_SECURITY_TYPE_FT_EAP_SHA384:
	case WIFI_SECURITY_TYPE_WEP:
	case WIFI_SECURITY_TYPE_WEP_OPEN:
	case WIFI_SECURITY_TYPE_WEP_SHARED:
		return -ENOTSUP;
	default:
		/* Mixed WPA/WPA2 scan results often arrive as UNKNOWN;
		 * ESP32 then rejects connect. PSK is the working STA mode. */
		*out = WIFI_SECURITY_TYPE_PSK;
		return 0;
	}
}

uint32_t aster_plat_wifi_security_class(uint8_t security)
{
	switch (security) {
	case WIFI_SECURITY_TYPE_NONE:
		return ASTER_PLAT_WIFI_SECURITY_OPEN;
	case WIFI_SECURITY_TYPE_PSK:
	case WIFI_SECURITY_TYPE_PSK_SHA256:
	case WIFI_SECURITY_TYPE_SAE:
	case WIFI_SECURITY_TYPE_SAE_H2E:
	case WIFI_SECURITY_TYPE_SAE_AUTO:
	case WIFI_SECURITY_TYPE_WPA_PSK:
	case WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL:
	case WIFI_SECURITY_TYPE_FT_PSK:
	case WIFI_SECURITY_TYPE_FT_SAE:
	case WIFI_SECURITY_TYPE_SAE_EXT_KEY:
		return ASTER_PLAT_WIFI_SECURITY_PERSONAL;
	case WIFI_SECURITY_TYPE_EAP:
	case WIFI_SECURITY_TYPE_EAP_PEAP_MSCHAPV2:
	case WIFI_SECURITY_TYPE_EAP_PEAP_GTC:
	case WIFI_SECURITY_TYPE_EAP_TTLS_MSCHAPV2:
	case WIFI_SECURITY_TYPE_EAP_PEAP_TLS:
	case WIFI_SECURITY_TYPE_FT_EAP:
	case WIFI_SECURITY_TYPE_FT_EAP_SHA384:
		return ASTER_PLAT_WIFI_SECURITY_ENTERPRISE;
	case WIFI_SECURITY_TYPE_WEP:
	case WIFI_SECURITY_TYPE_WEP_OPEN:
	case WIFI_SECURITY_TYPE_WEP_SHARED:
		return ASTER_PLAT_WIFI_SECURITY_WEP;
	default:
		return ASTER_PLAT_WIFI_SECURITY_OTHER;
	}
}

uint32_t aster_plat_wifi_state_class(int state)
{
	switch (state) {
	case WIFI_STATE_DISCONNECTED:
		return ASTER_PLAT_WIFI_STATE_DISCONNECTED;
	case WIFI_STATE_INTERFACE_DISABLED:
		return ASTER_PLAT_WIFI_STATE_DISABLED;
	case WIFI_STATE_INACTIVE:
		return ASTER_PLAT_WIFI_STATE_INACTIVE;
	case WIFI_STATE_SCANNING:
		return ASTER_PLAT_WIFI_STATE_SCANNING;
	case WIFI_STATE_AUTHENTICATING:
		return ASTER_PLAT_WIFI_STATE_AUTHENTICATING;
	case WIFI_STATE_ASSOCIATING:
		return ASTER_PLAT_WIFI_STATE_ASSOCIATING;
	case WIFI_STATE_ASSOCIATED:
		return ASTER_PLAT_WIFI_STATE_ASSOCIATED;
	case WIFI_STATE_4WAY_HANDSHAKE:
		return ASTER_PLAT_WIFI_STATE_FOUR_WAY;
	case WIFI_STATE_GROUP_HANDSHAKE:
		return ASTER_PLAT_WIFI_STATE_GROUP_HANDSHAKE;
	case WIFI_STATE_COMPLETED:
		return ASTER_PLAT_WIFI_STATE_COMPLETED;
	default:
		return ASTER_PLAT_WIFI_STATE_UNKNOWN;
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
	if (ift != iface) return;

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
		if (atomic_get(&conn_wait_kind) == CONN_WAIT_CONNECT) {
			conn_status = st ? st->status : -EIO;
			k_sem_give(&conn_done);
		}
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		if (atomic_get(&conn_wait_kind) == CONN_WAIT_DISCONNECT) {
			conn_status = -ENOTCONN;
			k_sem_give(&conn_done);
		} else if (atomic_get(&conn_wait_kind) == CONN_WAIT_NONE &&
			   req_ssid[0]) {
			LOG_WRN("Wi-Fi link lost; queuing saved-profile reconnect");
			(void)queue_wifi_job(ASTER_PLAT_WIFI_OP_RECONNECT, NULL, NULL);
		}
		break;
	default:
		break;
	}
}

static void ipv4_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			 struct net_if *ift)
{
	ARG_UNUSED(cb);
	if (ift != iface) return;

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
	*count = 0;
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
	char join_ssid[ASTER_WIFI_SSID_MAX + 1];
	char join_psk[65] = {0};
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
		return -EINVAL;
	}
	plen = (psk && psk[0]) ? strlen(psk) : 0;
	if (plen && (plen < 8 || plen > 64)) {
		return -EINVAL;
	}
	memcpy(join_ssid, ssid, slen + 1);
	if (plen) {
		memcpy(join_psk, psk, plen);
	}

	k_mutex_lock(&wifi_mu, K_FOREVER);
	/* Treat connecting to the already active SSID as success. The ESP32
	 * driver can return -EREMOTEIO for a second connect request even though
	 * the current link is healthy; applications commonly retry after a
	 * timeout or revisit their network settings. Do not replace saved
	 * credentials here, since the link may have been established with a
	 * different (known-good) passphrase. */
	{
		aster_wifi_status_t current;

		if (aster_plat_wifi_status(&current) == 0) {
			if (current.state == WIFI_STATE_COMPLETED &&
			    strcmp(current.ssid, join_ssid) == 0) {
				k_mutex_unlock(&wifi_mu);
				return 0;
			}
			/* ESP32 rejects connect while already associated. Switching
			 * networks must finish disconnect before starting the join. */
			if (current.state == WIFI_STATE_COMPLETED ||
			    current.state == WIFI_STATE_ASSOCIATED) {
				k_mutex_unlock(&wifi_mu);
				err = aster_plat_wifi_disconnect();
				if (err) return err;
				k_mutex_lock(&wifi_mu, K_FOREVER);
			}
		}
	}
	params.ssid = (const uint8_t *)join_ssid;
	params.ssid_length = (uint8_t)slen;
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;
	params.mfp = WIFI_MFP_OPTIONAL;
	params.timeout = CONN_TO_MS / 1000;

	hit = scan_find(join_ssid);
	if (plen == 0) {
		params.security = WIFI_SECURITY_TYPE_NONE;
	} else if (hit >= 0) {
		err = sta_security(scan_aps[hit].security, &params.security);
		if (err) {
			k_mutex_unlock(&wifi_mu);
			return err;
		}
		params.psk = (const uint8_t *)join_psk;
		params.psk_length = (uint8_t)plen;
	} else {
		params.security = WIFI_SECURITY_TYPE_PSK;
		params.psk = (const uint8_t *)join_psk;
		params.psk_length = (uint8_t)plen;
	}

	while (k_sem_take(&conn_done, K_NO_WAIT) == 0) {
	}
	while (k_sem_take(&dhcp_done, K_NO_WAIT) == 0) {
	}
	conn_status = -EINPROGRESS;
	atomic_set(&conn_wait_kind, CONN_WAIT_CONNECT);
	err = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
	if (err) {
		atomic_set(&conn_wait_kind, CONN_WAIT_NONE);
		k_mutex_unlock(&wifi_mu);
		return err;
	}
	err = k_sem_take(&conn_done, K_MSEC(CONN_TO_MS));
	atomic_set(&conn_wait_kind, CONN_WAIT_NONE);
	if (err) {
		k_mutex_unlock(&wifi_mu);
		return -ETIMEDOUT;
	}
	if (conn_status) {
		err = conn_status > 0 ? -EIO : conn_status;
		k_mutex_unlock(&wifi_mu);
		return err;
	}

	/* Keep the last known-good credentials, not a failed request. The RAM
	 * copy powers reconnect/qpanel even when the card is later removed. */
	memcpy(req_ssid, join_ssid, slen + 1);
	memcpy(req_psk, join_psk, plen + 1);
	req_psk_len = plen;
	{
		wifi_profile_t profile = { .version = WIFI_PROFILE_VERSION,
			.psk_len = (uint32_t)plen };
		memcpy(profile.ssid, join_ssid, slen + 1);
		memcpy(profile.psk, join_psk, plen + 1);
		int save_err = aster_kv_set(WIFI_KV_ID, WIFI_KV_KEY,
					    &profile, sizeof(profile));

		if (save_err) {
			LOG_WRN("connected to %s but saving Wi-Fi profile failed: %d",
				join_ssid, save_err);
		}
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
	aster_wifi_status_t st;
	int err;

	err = wifi_ready();
	if (err) {
		return err;
	}
	if (aster_plat_wifi_status(&st) == 0 &&
	    st.state != WIFI_STATE_COMPLETED &&
	    st.state != WIFI_STATE_ASSOCIATED) {
		return 0;
	}
	k_mutex_lock(&wifi_mu, K_FOREVER);
	while (k_sem_take(&conn_done, K_NO_WAIT) == 0) {
	}
	conn_status = -EINPROGRESS;
	atomic_set(&conn_wait_kind, CONN_WAIT_DISCONNECT);
	err = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	if (!err) {
		err = k_sem_take(&conn_done, K_MSEC(DISC_TO_MS));
		if (err == 0 && conn_status == -ENOTCONN) {
			err = 0;
		} else if (err == 0) {
			err = conn_status ? conn_status : -EIO;
		} else if (err == -EAGAIN &&
			   aster_plat_wifi_status(&st) == 0 &&
			   st.state != WIFI_STATE_COMPLETED &&
			   st.state != WIFI_STATE_ASSOCIATED) {
			err = 0;
		}
	}
	atomic_set(&conn_wait_kind, CONN_WAIT_NONE);
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
	aster_wifi_status_t st;
	aster_wifi_ap_t aps[ASTER_WIFI_AP_MAX];
	int ap_count = 0;

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
	if (aster_plat_wifi_status(&st) == 0 &&
	    st.state == WIFI_STATE_COMPLETED && strcmp(st.ssid, ssid) == 0) {
		return 0;
	}
	/* Re-discover the saved AP's security mode after reboot so WPA3 personal
	 * networks do not get retried as WPA2 PSK by the fallback path. A failed
	 * scan still permits a hidden SSID to use the ordinary PSK attempt. */
	int scan_err = aster_plat_wifi_scan(aps, ARRAY_SIZE(aps), &ap_count);
	if (scan_err) {
		LOG_WRN("Wi-Fi reconnect scan failed: %d; trying saved credentials",
			scan_err);
	}

	/* NULL, not "", for an open AP: connect() derives its PSK length
	 * from the string and rejects anything shorter than 8 that is not
	 * empty. */
	return aster_plat_wifi_connect(ssid, have_psk ? psk : NULL);
}

int aster_plat_wifi_forget(void)
{
	aster_wifi_status_t st;
	char saved[ASTER_WIFI_SSID_MAX + 1];
	bool disconnect = false;
	int err;

	if (aster_plat_wifi_last_ssid(saved, sizeof(saved)) == 0 &&
	    aster_plat_wifi_status(&st) == 0 &&
	    st.state == WIFI_STATE_COMPLETED && strcmp(st.ssid, saved) == 0) {
		disconnect = true;
	}
	if (disconnect) {
		err = aster_plat_wifi_disconnect();
		if (err && err != -ENOTCONN) {
			return err;
		}
	}

	err = aster_kv_delete(WIFI_KV_ID, WIFI_KV_KEY);
	if (err) {
		return err;
	}
	k_mutex_lock(&wifi_mu, K_FOREVER);
	memset(req_ssid, 0, sizeof(req_ssid));
	memset(req_psk, 0, sizeof(req_psk));
	req_psk_len = 0;
	k_mutex_unlock(&wifi_mu);
	return 0;
}

void aster_plat_wifi_hotspot_status(aster_wifi_hotspot_status_t *status)
{
	if (!status) return;
	k_mutex_lock(&job_mu, K_FOREVER);
	memcpy(status->ssid, hotspot_profile.ssid, sizeof(status->ssid));
	status->enabled = hotspot_enabled;
	status->configured = hotspot_configured;
	k_mutex_unlock(&job_mu);
}

static int hotspot_stop(void)
{
#if defined(CONFIG_NET_DHCPV4_SERVER) && defined(CONFIG_WIFI_USAGE_MODE_STA_AP)
	struct net_if *ap = net_if_get_wifi_sap();
	if (!ap) return -ENODEV;
	if (!hotspot_enabled) return 0;
	int err = net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, ap, NULL, 0);
	if (err) return err;
	(void)net_dhcpv4_server_stop(ap);
	k_mutex_lock(&job_mu, K_FOREVER);
	hotspot_enabled = false;
	k_mutex_unlock(&job_mu);
	return 0;
#else
	return -ENOTSUP;
#endif
}

static int hotspot_start(const char *ssid, const char *psk)
{
#if defined(CONFIG_NET_DHCPV4_SERVER) && defined(CONFIG_WIFI_USAGE_MODE_STA_AP)
	struct net_if *ap = net_if_get_wifi_sap();
	struct net_in_addr addr, mask, pool;
	int err;
	if (!ap) return -ENODEV;
	if (hotspot_enabled) {
		err = hotspot_stop();
		if (err) return err;
	}
	err = net_if_up(ap);
	if (err && err != -EALREADY) return err;
	net_addr_pton(NET_AF_INET, "192.168.4.1", &addr);
	net_addr_pton(NET_AF_INET, "255.255.255.0", &mask);
	net_addr_pton(NET_AF_INET, "192.168.4.10", &pool);
	if (!net_if_ipv4_addr_add(ap, &addr, NET_ADDR_MANUAL, 0) ||
	    !net_if_ipv4_set_netmask_by_addr(ap, &addr, &mask)) return -EIO;
	err = net_dhcpv4_server_start(ap, &pool);
	if (err) return err;
	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)ssid, .ssid_length = strlen(ssid),
		.psk = (const uint8_t *)psk, .psk_length = strlen(psk),
		.security = WIFI_SECURITY_TYPE_PSK, .channel = 6,
		.band = WIFI_FREQ_BAND_2_4_GHZ, .mfp = WIFI_MFP_OPTIONAL,
	};
	err = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap, &params, sizeof(params));
	if (err) {
		(void)net_dhcpv4_server_stop(ap);
		return err;
	}
	wifi_profile_t profile = {.version = WIFI_PROFILE_VERSION, .psk_len = strlen(psk)};
	strcpy(profile.ssid, ssid);
	strcpy(profile.psk, psk);
	k_mutex_lock(&job_mu, K_FOREVER);
	hotspot_profile = profile;
	hotspot_enabled = true;
	hotspot_configured = true;
	k_mutex_unlock(&job_mu);
	err = aster_kv_set(WIFI_KV_ID, "hotspot", &profile, sizeof(profile));
	if (err) LOG_WRN("hotspot enabled but saving configuration failed: %d", err);
	return 0;
#else
	ARG_UNUSED(ssid);
	ARG_UNUSED(psk);
	return -ENOTSUP;
#endif
}

int aster_plat_wifi_hotspot_start(const char *ssid, const char *psk)
{
	return queue_wifi_job(ASTER_PLAT_WIFI_OP_HOTSPOT_START, ssid, psk);
}

int aster_plat_wifi_hotspot_stop(void)
{
	return queue_wifi_job(ASTER_PLAT_WIFI_OP_HOTSPOT_STOP, NULL, NULL);
}

static void hotspot_load(void)
{
	wifi_profile_t profile = {0};
	size_t len = 0;
	int err = aster_kv_get(WIFI_KV_ID, "hotspot", &profile, sizeof(profile), &len);
	if (err == -ENOENT) return;
	if (err) {
		LOG_WRN("saved hotspot read failed: %d", err);
		return;
	}
	size_t slen = strnlen(profile.ssid, sizeof(profile.ssid));
	if (len != sizeof(profile) || profile.version != WIFI_PROFILE_VERSION ||
	    !slen || slen > ASTER_WIFI_SSID_MAX || profile.psk_len < 8 ||
	    profile.psk_len > 63 || strnlen(profile.psk, sizeof(profile.psk)) != profile.psk_len) {
		LOG_WRN("saved hotspot configuration is invalid");
		return;
	}
	k_mutex_lock(&job_mu, K_FOREVER);
	hotspot_profile = profile;
	hotspot_configured = true;
	k_mutex_unlock(&job_mu);
}

static int queue_wifi_job(uint32_t op, const char *ssid, const char *psk)
{
	int err = 0;

	k_mutex_lock(&job_mu, K_FOREVER);
	if (!job_q_up) {
		k_work_queue_start(&job_q, job_stack,
				   K_THREAD_STACK_SIZEOF(job_stack),
				   K_PRIO_PREEMPT(12), NULL);
		job_q_up = true;
	}
	if (job_busy) {
		k_mutex_unlock(&job_mu);
		return -EBUSY;
	}
	if (op == ASTER_PLAT_WIFI_OP_HOTSPOT_START) {
		const char *name = ssid ? ssid : hotspot_profile.ssid;
		const char *password = psk && psk[0] ? psk : hotspot_profile.psk;
		if (!name[0] || strlen(name) > ASTER_WIFI_SSID_MAX ||
		    strlen(password) < 8 || strlen(password) > 63) {
			k_mutex_unlock(&job_mu);
			return -EINVAL;
		}
		strcpy(job_ssid, name);
		strcpy(job_psk, password);
		job_psk_len = strlen(password);
	} else if (op == ASTER_PLAT_WIFI_OP_CONNECT) {
		if (!ssid || !ssid[0] || strlen(ssid) > ASTER_WIFI_SSID_MAX ||
		    (psk && psk[0] && (strlen(psk) < 8 || strlen(psk) > 64))) {
			k_mutex_unlock(&job_mu);
			return -EINVAL;
		}
		strncpy(job_ssid, ssid, sizeof(job_ssid) - 1);
		job_ssid[sizeof(job_ssid) - 1] = 0;
		if (psk) {
			strncpy(job_psk, psk, sizeof(job_psk) - 1);
			job_psk[sizeof(job_psk) - 1] = 0;
		} else {
			job_psk[0] = 0;
		}
		job_psk_len = strlen(job_psk);
	} else if (op == ASTER_PLAT_WIFI_OP_SCAN) {
		job_scan_valid = false;
		job_scan_n = 0;
	}
	job_op = op;
	job_busy = true;
	job_last_result = -EINPROGRESS;
	if (k_work_submit_to_queue(&job_q, &wifi_job) < 0) {
		job_busy = false;
		job_op = ASTER_PLAT_WIFI_OP_NONE;
		err = -EBUSY;
	}
	k_mutex_unlock(&job_mu);
	return err;
}

static void wifi_job_fn(struct k_work *work)
{
	aster_wifi_ap_t found[ASTER_WIFI_AP_MAX];
	char ssid[ASTER_WIFI_SSID_MAX + 1];
	char psk[65];
	size_t psk_len;
	uint32_t op;
	int count = 0;
	int err;

	ARG_UNUSED(work);
	k_mutex_lock(&job_mu, K_FOREVER);
	op = job_op;
	memcpy(ssid, job_ssid, sizeof(ssid));
	memcpy(psk, job_psk, sizeof(psk));
	psk_len = job_psk_len;
	k_mutex_unlock(&job_mu);

	switch (op) {
	case ASTER_PLAT_WIFI_OP_SCAN:
		err = aster_plat_wifi_scan(found, ARRAY_SIZE(found), &count);
		break;
	case ASTER_PLAT_WIFI_OP_CONNECT:
		err = aster_plat_wifi_connect(ssid, psk_len ? psk : NULL);
		break;
	case ASTER_PLAT_WIFI_OP_DISCONNECT:
		err = aster_plat_wifi_disconnect();
		break;
	case ASTER_PLAT_WIFI_OP_RECONNECT:
		err = aster_plat_wifi_reconnect();
		break;
	case ASTER_PLAT_WIFI_OP_HOTSPOT_START:
		err = hotspot_start(ssid, psk);
		break;
	case ASTER_PLAT_WIFI_OP_HOTSPOT_STOP:
		err = hotspot_stop();
		break;
	case ASTER_PLAT_WIFI_OP_FORGET:
		err = aster_plat_wifi_forget();
		break;
	default:
		err = -EINVAL;
		break;
	}

	k_mutex_lock(&job_mu, K_FOREVER);
	if (op == ASTER_PLAT_WIFI_OP_SCAN && err == 0) {
		memcpy(job_scan, found, (size_t)count * sizeof(found[0]));
		job_scan_n = count;
		job_scan_valid = true;
	}
	job_last_op = op;
	job_last_result = err;
	job_op = ASTER_PLAT_WIFI_OP_NONE;
	job_busy = false;
	k_mutex_unlock(&job_mu);
	if (err) {
		LOG_WRN("async operation %u failed: %d", (unsigned)op, err);
	} else {
		LOG_INF("async operation %u completed", (unsigned)op);
	}
}

int aster_plat_wifi_scan_start(void)
{
	return queue_wifi_job(ASTER_PLAT_WIFI_OP_SCAN, NULL, NULL);
}

int aster_plat_wifi_scan_results(aster_wifi_ap_t *out, int max, int *count)
{
	if (!out || max <= 0 || !count) {
		return -EINVAL;
	}
	*count = 0;
	k_mutex_lock(&job_mu, K_FOREVER);
	if (job_busy && job_op == ASTER_PLAT_WIFI_OP_SCAN) {
		k_mutex_unlock(&job_mu);
		return -EAGAIN;
	}
	if (!job_scan_valid) {
		k_mutex_unlock(&job_mu);
		return -ENOENT;
	}
	if (max > job_scan_n) {
		max = job_scan_n;
	}
	memcpy(out, job_scan, (size_t)max * sizeof(job_scan[0]));
	*count = max;
	k_mutex_unlock(&job_mu);
	return 0;
}

int aster_plat_wifi_connect_start(const char *ssid, const char *psk)
{
	return queue_wifi_job(ASTER_PLAT_WIFI_OP_CONNECT, ssid, psk);
}

int aster_plat_wifi_disconnect_start(void)
{
	return queue_wifi_job(ASTER_PLAT_WIFI_OP_DISCONNECT, NULL, NULL);
}

int aster_plat_wifi_reconnect_start(void)
{
	return queue_wifi_job(ASTER_PLAT_WIFI_OP_RECONNECT, NULL, NULL);
}

int aster_plat_wifi_forget_start(void)
{
	return queue_wifi_job(ASTER_PLAT_WIFI_OP_FORGET, NULL, NULL);
}

void aster_plat_wifi_operation(uint32_t *operation, uint32_t *last_operation,
			       int *last_result)
{
	k_mutex_lock(&job_mu, K_FOREVER);
	if (operation) {
		*operation = job_busy ? job_op : ASTER_PLAT_WIFI_OP_NONE;
	}
	if (last_operation) {
		*last_operation = job_last_op;
	}
	if (last_result) {
		*last_result = job_last_result;
	}
	k_mutex_unlock(&job_mu);
}

int aster_plat_wifi_storage_ready(void)
{
	hotspot_load();
	wifi_profile_t profile;
	size_t len = 0;
	size_t ssid_len, psk_len;
	int err;

	memset(&profile, 0, sizeof(profile));
	err = aster_kv_get(WIFI_KV_ID, WIFI_KV_KEY, &profile,
			   sizeof(profile), &len);
	if (err == -ENOENT) {
		return 0;
	}
	if (err) {
		LOG_WRN("saved Wi-Fi profile read failed: %d", err);
		return err;
	}
	ssid_len = strnlen(profile.ssid, sizeof(profile.ssid));
	psk_len = profile.psk_len;
	if (len != sizeof(profile) || profile.version != WIFI_PROFILE_VERSION ||
	    ssid_len == 0 || ssid_len > ASTER_WIFI_SSID_MAX ||
	    psk_len > 64 || (psk_len != 0 && psk_len < 8) ||
	    profile.ssid[ssid_len] != 0 || profile.psk[psk_len] != 0) {
		LOG_WRN("saved Wi-Fi profile is invalid; forgetting it");
		(void)aster_kv_delete(WIFI_KV_ID, WIFI_KV_KEY);
		return -EINVAL;
	}
	k_mutex_lock(&wifi_mu, K_FOREVER);
	memcpy(req_ssid, profile.ssid, ssid_len + 1);
	memcpy(req_psk, profile.psk, psk_len + 1);
	req_psk_len = psk_len;
	k_mutex_unlock(&wifi_mu);
	LOG_INF("saved Wi-Fi profile loaded for %s", req_ssid);
	return queue_wifi_job(ASTER_PLAT_WIFI_OP_RECONNECT, NULL, NULL);
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
	st->security = (uint8_t)ws.security;
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
