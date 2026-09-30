/* SPDX-License-Identifier: MIT */
/* Firmware-owned wall clock and background SNTP synchronization. */
#include "aster_time.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/clock.h>

#ifdef CONFIG_SNTP
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/sntp.h>
#endif

LOG_MODULE_REGISTER(aster_time, LOG_LEVEL_INF);

static atomic_t s_time_synced;
static atomic_t s_sync_error = ATOMIC_INIT(-EAGAIN);

#ifdef CONFIG_SNTP
#define TIME_SYNC_TIMEOUT_MS 4000
#define TIME_SYNC_RETRY      K_SECONDS(60)
#define TIME_SYNC_PERIODIC   K_HOURS(6)

/* DNS and a socket timeout must never stall the UI's system work queue. */
K_THREAD_STACK_DEFINE(s_time_sync_stack, 3072);
static struct k_work_q s_time_sync_queue;
static struct k_work_delayable s_time_sync_work;
static struct net_mgmt_event_callback s_time_net_cb;

static void time_sync_handler(struct k_work *work)
{
	static const char *const servers[] = {
		"ntp.aliyun.com",
		"time.cloudflare.com",
	};
	struct sntp_time ntp = {0};
	struct timespec ts;
	int err = -EHOSTUNREACH;

	ARG_UNUSED(work);

	for (size_t i = 0; i < ARRAY_SIZE(servers); i++) {
		err = sntp_simple(servers[i], TIME_SYNC_TIMEOUT_MS, &ntp);
		if (err == 0) {
			break;
		}
		LOG_WRN("SNTP %s failed: %d", servers[i], err);
	}

	if (err == 0) {
		ts.tv_sec = (time_t)ntp.seconds;
		ts.tv_nsec = (long)(((uint64_t)ntp.fraction * 1000000000ULL) >> 32);
		err = sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
	}

	atomic_set(&s_sync_error, err);
	if (err == 0) {
		atomic_set(&s_time_synced, 1);
		LOG_INF("wall clock synchronized (%lld UTC)",
			(long long)ntp.seconds);
		k_work_reschedule_for_queue(&s_time_sync_queue, &s_time_sync_work,
					    TIME_SYNC_PERIODIC);
	} else {
		LOG_WRN("wall clock sync failed: %d; retrying", err);
		k_work_reschedule_for_queue(&s_time_sync_queue, &s_time_sync_work,
					    TIME_SYNC_RETRY);
	}
}

static void time_net_event(struct net_mgmt_event_callback *cb,
			   uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_IPV4_DHCP_BOUND) {
		/* Let DHCP finish publishing its DNS server before resolving NTP. */
		k_work_reschedule_for_queue(&s_time_sync_queue, &s_time_sync_work,
					    K_SECONDS(2));
	}
}
#endif /* CONFIG_SNTP */

int aster_time_read(aster_time_info_t *out)
{
	struct timespec now;
	struct tm local;
	time_t local_seconds;
	int err;

	if (!out) {
		return -EINVAL;
	}

	err = sys_clock_gettime(SYS_CLOCK_REALTIME, &now);
	if (err) {
		return err;
	}

	local_seconds = (time_t)((int64_t)now.tv_sec +
		(int64_t)CONFIG_ASTER_TIMEZONE_OFFSET_MINUTES * 60);
	if (!gmtime_r(&local_seconds, &local)) {
		return -ERANGE;
	}

	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);
	out->version = 1;
	out->unix_ms = (uint64_t)now.tv_sec * 1000ULL +
		(uint64_t)now.tv_nsec / 1000000ULL;
	out->flags = atomic_get(&s_time_synced) ? ASTER_TIME_FLAG_SYNCED : 0;
	out->year = local.tm_year + 1900;
	out->month = (uint32_t)local.tm_mon + 1;
	out->day = (uint32_t)local.tm_mday;
	out->hour = (uint32_t)local.tm_hour;
	out->minute = (uint32_t)local.tm_min;
	out->second = (uint32_t)local.tm_sec;
	out->weekday = (uint32_t)local.tm_wday;
	return 0;
}

static int aster_time_init(void)
{
	const struct timespec fallback = {
		.tv_sec = (time_t)CONFIG_ASTER_TIME_DEFAULT_EPOCH,
		.tv_nsec = 0,
	};
	int err = sys_clock_settime(SYS_CLOCK_REALTIME, &fallback);

	if (err) {
		LOG_ERR("could not set fallback wall clock: %d", err);
	}

#ifdef CONFIG_SNTP
	k_work_queue_start(&s_time_sync_queue, s_time_sync_stack,
			   K_THREAD_STACK_SIZEOF(s_time_sync_stack),
			   K_PRIO_PREEMPT(8), NULL);
	k_work_init_delayable(&s_time_sync_work, time_sync_handler);
	net_mgmt_init_event_callback(&s_time_net_cb, time_net_event,
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&s_time_net_cb);
	/* Also cover an already-bound interface and boards without Wi-Fi events. */
	k_work_schedule_for_queue(&s_time_sync_queue, &s_time_sync_work,
			  K_SECONDS(5));
#endif
	return 0;
}

SYS_INIT(aster_time_init, APPLICATION, 85);
