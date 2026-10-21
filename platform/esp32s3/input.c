#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * Atomic touch frames and Zephyr key events -> ASTER input model.
 */
#include "aster_platform.h"
#include "touch_state.h"

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(aster_input, LOG_LEVEL_INF);

static struct aster_touch_state touch;
static bool touch_ready;

/* --- key state ----------------------------------------------------------- */
#define KEY_EDGE_QUEUE 16
struct key_edge {
	uint16_t code;
	bool     pressed;
};
static struct {
	struct key_edge q[KEY_EDGE_QUEUE];
	int head;      /* next write slot */
	int count;     /* queued edges */
	uint16_t held[ASTER_PLAT_MAX_KEYS];
	int held_n;
	bool available; /* saw at least one non-touch EV_KEY */
} ks;

static K_MUTEX_DEFINE(input_lock);

static void key_push_locked(uint16_t code, bool pressed)
{
	/* coalesce duplicate consecutive edges of the same code */
	if (ks.count > 0) {
		struct key_edge *last =
			&ks.q[(ks.head - 1 + KEY_EDGE_QUEUE) % KEY_EDGE_QUEUE];
		if (last->code == code && last->pressed == pressed) {
			return;
		}
	}
	if (ks.count == KEY_EDGE_QUEUE) {
		ks.head = (ks.head + 1) % KEY_EDGE_QUEUE; /* drop oldest */
		ks.count--;
	}
	ks.q[ks.head].code = code;
	ks.q[ks.head].pressed = pressed;
	ks.head = (ks.head + 1) % KEY_EDGE_QUEUE;
	ks.count++;

	/* maintain held-key set */
	int i;
	for (i = 0; i < ks.held_n; i++) {
		if (ks.held[i] == code) {
			break;
		}
	}
	if (pressed && i == ks.held_n && ks.held_n < ASTER_PLAT_MAX_KEYS) {
		ks.held[ks.held_n++] = code;
	} else if (!pressed && i < ks.held_n) {
		ks.held[i] = ks.held[--ks.held_n];
	}
}

static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	k_mutex_lock(&input_lock, K_FOREVER);
	switch (evt->type) {
	case INPUT_EV_ABS:
		/* Touch drivers publish complete frames through the atomic
		 * aster_plat_input_touch_report_frame() interface below. */
		break;
	case INPUT_EV_KEY:
		if (evt->code != INPUT_BTN_TOUCH && evt->code < 0xFFFF) {
			key_push_locked((uint16_t)evt->code, evt->value != 0);
			ks.available = true;
		}
		break;
	default:
		break;
	}
	k_mutex_unlock(&input_lock);
}

INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

int aster_input_platform_init(void)
{
	return 0;
}

bool aster_plat_input_touch_available(void)
{
	/* Follows the menuconfig/capability choice (which also selects the
	 * driver). A DT-only test would keep advertising touch when the build
	 * explicitly turned the hardware off. */
	return IS_ENABLED(CONFIG_ASTER_HAS_TOUCH) && touch_ready &&
	       aster_plat_display_count() > 0;
}

uint32_t aster_plat_input_touch_max_points(void)
{
	return aster_plat_input_touch_available() ? ASTER_PLAT_MAX_TOUCH : 0u;
}

bool aster_plat_input_keys_available(void)
{
	/* Must stay consistent with the ASTER_FEATURE_KEYBOARD profile flag:
	 * the board declares keys in Kconfig, and the port confirms the DT
	 * buttons node (or a first observed edge) actually backs it up. */
	if (!IS_ENABLED(CONFIG_ASTER_HAS_KEYS)) {
		return false;
	}
	return ks.available || DT_NODE_EXISTS(DT_PATH(buttons));
}

int aster_plat_input_poll(aster_touch_evt_t *evt, int max)
{
	if (!evt || max <= 0 || !aster_plat_input_touch_available()) return 0;
	struct aster_touch_event raw[ASTER_TOUCH_EVENT_QUEUE];
	if (max > ASTER_TOUCH_EVENT_QUEUE) max = ASTER_TOUCH_EVENT_QUEUE;
	k_mutex_lock(&input_lock, K_FOREVER);
	int n = aster_touch_state_poll(&touch, raw, max);
	k_mutex_unlock(&input_lock);
	/* Touch arrives exactly as the digitizer reported it: the DT touch node
	 * is deliberately neutral, so Zephyr's input_touch.c applied nothing on
	 * the way through. Turning that into LOGICAL space is the platform's
	 * job, and it lives in display.c next to the window it depends on. */
	for (int i = 0; i < n; i++) {
		evt[i].x = raw[i].x;
		evt[i].y = raw[i].y;
		evt[i].action = raw[i].action;
		evt[i].slot = raw[i].slot;
		evt[i].contact_mask = raw[i].contact_mask;
		aster_plat_display_touch_map(&evt[i].x, &evt[i].y);
	}
	return n;
}

void aster_plat_input_state(uint32_t *points, uint32_t max)
{
	if (!points || max == 0) return;
	k_mutex_lock(&input_lock, K_FOREVER);
	if (aster_plat_input_touch_available()) {
		aster_touch_state_snapshot(&touch, points, max);
	} else {
		points[2] = ASTER_INPUT_TERM;
	}
	k_mutex_unlock(&input_lock);
	/* Snapshot entries are packed as 4-u32 x/y/action/slot records. */
	for (uint32_t i = 0; i < max && i < ASTER_PLAT_MAX_TOUCH &&
	     points[i * 4u + 2u] != ASTER_INPUT_TERM; i++) {
		aster_plat_display_touch_map(&points[i * 4u], &points[i * 4u + 1u]);
	}
}

/* --- keys --------------------------------------------------------------- */

int aster_plat_input_keys_poll(aster_key_evt_t *evt, int max)
{
	int n = 0;
	k_mutex_lock(&input_lock, K_FOREVER);
	while (ks.count > 0 && n < max) {
		evt[n].code = ks.q[(ks.head - ks.count + KEY_EDGE_QUEUE) % KEY_EDGE_QUEUE].code;
		evt[n].pressed = ks.q[(ks.head - ks.count + KEY_EDGE_QUEUE) % KEY_EDGE_QUEUE].pressed;
		ks.count--;
		n++;
	}
	k_mutex_unlock(&input_lock);
	return n;
}

int aster_plat_input_keys_state(uint32_t *codes, int max)
{
	int n = 0;
	k_mutex_lock(&input_lock, K_FOREVER);
	for (int i = 0; i < ks.held_n && n < max; i++) {
		codes[n++] = ks.held[i];
	}
	k_mutex_unlock(&input_lock);
	return n;
}

void aster_plat_input_keys_inject(uint32_t code, bool pressed)
{
	k_mutex_lock(&input_lock, K_FOREVER);
	key_push_locked((uint16_t)code, pressed);
	ks.available = true;
	k_mutex_unlock(&input_lock);
}

/* --- touch --------------------------------------------------------------- */

void aster_plat_input_touch_inject(uint32_t x, uint32_t y, uint32_t action)
{
	/* LOGICAL (what the app lays out in, i.e. what a screenshot shows) ->
	 * raw digitizer report. Injecting in the raw space and letting the
	 * normal poll path map it forward is deliberate: the point then travels
	 * the same road a finger's does, so it cannot agree with the display by
	 * accident. The slot helper performs the single inverse map before it
	 * enters the shared state machine. */
	aster_plat_input_touch_inject_slot(0, x, y, action);
}

void aster_plat_input_touch_inject_slot(uint32_t slot, uint32_t x, uint32_t y,
					uint32_t action)
{
	aster_plat_display_touch_unmap(&x, &y);
	k_mutex_lock(&input_lock, K_FOREVER);
	aster_touch_state_inject(&touch, slot, x, y, action);
	k_mutex_unlock(&input_lock);
}

void aster_plat_input_touch_report_frame(const struct aster_touch_frame *frame)
{
	if (!frame) return;
	k_mutex_lock(&input_lock, K_FOREVER);
	(void)aster_touch_state_report_frame(&touch, frame);
	k_mutex_unlock(&input_lock);
}

void aster_plat_input_touch_debug(aster_touch_debug_t *out)
{
	if (!out) return;
	k_mutex_lock(&input_lock, K_FOREVER);
	*out = (aster_touch_debug_t) {
		.recovery=touch.ignore_until_clear, .errors=touch.io_errors,
		.pending=touch.event_count,
	};
	for (uint32_t i=0; i<ASTER_TOUCH_SLOT_COUNT; i++) {
		if (!touch.slots[i].down) continue;
		if (touch.slots[i].id <= ASTER_TOUCH_HW_ID_MAX) out->hardware_mask |= 1u<<i;
		else out->synthetic_mask |= 1u<<i;
	}
	k_mutex_unlock(&input_lock);
}

void aster_plat_input_touch_report_error(uint64_t now_ms)
{
	k_mutex_lock(&input_lock, K_FOREVER);
	aster_touch_state_report_error(&touch, now_ms);
	k_mutex_unlock(&input_lock);
}

void aster_plat_input_touch_set_ready(bool ready)
{
	k_mutex_lock(&input_lock, K_FOREVER);
	touch_ready = ready;
	if (!ready) aster_touch_state_clear(&touch);
	k_mutex_unlock(&input_lock);
}
