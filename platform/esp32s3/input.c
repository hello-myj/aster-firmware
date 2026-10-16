#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT
 * Input: Zephyr input events -> ASTER touch + key models.
 * Touch (ft5336): driver reports input_touchscreen_report_pos(x,y) then
 * INPUT_BTN_TOUCH 1/0 (EV_KEY). We keep the latest coords and emit
 * down/move/up edge events.
 * Keys (gpio-keys etc., any EV_KEY that is not BTN_TOUCH): edges are queued
 * and held codes tracked; drained via aster_plat_input_keys_poll().
 */
#include "aster_platform.h"

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(aster_input, LOG_LEVEL_INF);

static struct {
	uint16_t x;
	uint16_t y;
	uint16_t down_x, down_y; /* immutable until pending DOWN is consumed */
	bool     down;
	bool     pending_down;
	bool     pending_up;
	bool     pending_move;
	bool     has_pos; /* a position was reported since last press */
} pt;

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

static K_MUTEX_DEFINE(pt_lock);

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

	k_mutex_lock(&pt_lock, K_FOREVER);
	switch (evt->type) {
	case INPUT_EV_ABS:
		if (evt->code == INPUT_ABS_X) {
			pt.x = (uint16_t)evt->value;
			pt.has_pos = true;
			if (pt.down) {
				pt.pending_move = true;
			}
		} else if (evt->code == INPUT_ABS_Y) {
			pt.y = (uint16_t)evt->value;
			pt.has_pos = true;
			if (pt.down) {
				pt.pending_move = true;
			}
		}
		break;
	case INPUT_EV_KEY:
		if (evt->code == INPUT_BTN_TOUCH) {
			if (evt->value) { /* pressed */
				if (!pt.down) {
					pt.down = true;
					if (!pt.pending_down) { pt.down_x = pt.x; pt.down_y = pt.y; }
					pt.pending_down = true;
				}
				pt.pending_move = true;
			} else { /* released */
				if (pt.down) {
					pt.down = false;
					pt.pending_up = true;
				}
			}
		} else if (evt->code < 0xFFFF) {
			key_push_locked((uint16_t)evt->code, evt->value != 0);
			ks.available = true;
		}
		break;
	default:
		break;
	}
	k_mutex_unlock(&pt_lock);
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
	return IS_ENABLED(CONFIG_ASTER_HAS_TOUCH) && aster_plat_display_count() > 0;
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
    if (!aster_plat_input_touch_available()) {
        k_mutex_lock(&pt_lock, K_FOREVER);
        pt.pending_down = pt.pending_move = pt.pending_up = false;
        pt.down = false;
        k_mutex_unlock(&pt_lock);
        return 0;
    }
	int n = 0;
	k_mutex_lock(&pt_lock, K_FOREVER);
	if (pt.pending_down && n < max) {
		evt[n].x = pt.down_x;
		evt[n].y = pt.down_y;
		evt[n].action = ASTER_TOUCH_DOWN;
		pt.pending_down = false;
		n++;
	}
	/* Preserve the last MOVE before UP, even if a whole swipe arrived
	 * while the APP was drawing. The next poll drains UP when max is 1/2. */
	if (pt.pending_move && !pt.pending_down && n < max) {
		evt[n].x = pt.x;
		evt[n].y = pt.y;
		evt[n].action = ASTER_TOUCH_MOVE;
		pt.pending_move = false;
		n++;
	}
	if (pt.pending_up && n < max) {
		evt[n].x = pt.x;
		evt[n].y = pt.y;
		evt[n].action = ASTER_TOUCH_UP;
		pt.pending_up = false;
		pt.pending_move = false;
		n++;
	}
	k_mutex_unlock(&pt_lock);
	/* Touch arrives exactly as the digitizer reported it: the DT touch node
	 * is deliberately neutral, so Zephyr's input_touch.c applied nothing on
	 * the way through. Turning that into LOGICAL space is the platform's
	 * job, and it lives in display.c next to the window it depends on. */
	for (int i = 0; i < n; i++) {
		aster_plat_display_touch_map(&evt[i].x, &evt[i].y);
	}
	return n;
}

void aster_plat_input_state(uint32_t *points, uint32_t max)
{
	k_mutex_lock(&pt_lock, K_FOREVER);
	if (max > 0 && pt.down && aster_plat_input_touch_available()) {
		points[0] = pt.x;
		points[1] = pt.y;
		points[2] = ASTER_TOUCH_DOWN;
		points[3] = 0;
		points[4] = 0;
		points[5] = 0;
		points[6] = 0;
		points[7] = ASTER_INPUT_TERM; /* terminator action */
	} else if (max > 0) {
		points[0] = pt.x;
		points[1] = pt.y;
		points[2] = ASTER_INPUT_TERM;
		points[3] = 0;
	}
	k_mutex_unlock(&pt_lock);
	/* same raw->logical mapping as aster_plat_input_poll() */
	if (max > 0) {
		uint32_t x = points[0], y = points[1];
		aster_plat_display_touch_map(&x, &y);
		points[0] = x;
		points[1] = y;
	}
}

/* --- keys --------------------------------------------------------------- */

int aster_plat_input_keys_poll(aster_key_evt_t *evt, int max)
{
	int n = 0;
	k_mutex_lock(&pt_lock, K_FOREVER);
	while (ks.count > 0 && n < max) {
		evt[n].code = ks.q[(ks.head - ks.count + KEY_EDGE_QUEUE) % KEY_EDGE_QUEUE].code;
		evt[n].pressed = ks.q[(ks.head - ks.count + KEY_EDGE_QUEUE) % KEY_EDGE_QUEUE].pressed;
		ks.count--;
		n++;
	}
	k_mutex_unlock(&pt_lock);
	return n;
}

int aster_plat_input_keys_state(uint32_t *codes, int max)
{
	int n = 0;
	k_mutex_lock(&pt_lock, K_FOREVER);
	for (int i = 0; i < ks.held_n && n < max; i++) {
		codes[n++] = ks.held[i];
	}
	k_mutex_unlock(&pt_lock);
	return n;
}

void aster_plat_input_keys_inject(uint32_t code, bool pressed)
{
	k_mutex_lock(&pt_lock, K_FOREVER);
	key_push_locked((uint16_t)code, pressed);
	ks.available = true;
	k_mutex_unlock(&pt_lock);
}

/* --- touch --------------------------------------------------------------- */

void aster_plat_input_touch_inject(uint32_t x, uint32_t y, uint32_t action)
{
	/* LOGICAL (what the app lays out in, i.e. what a screenshot shows) ->
	 * raw digitizer report. Injecting in the raw space and letting the
	 * normal poll path map it forward is deliberate: the point then travels
	 * the same road a finger's does, so it cannot agree with the display by
	 * accident.
	 *
	 * This used to be a hand-written inverse of the rotation, right here.
	 * It is now the platform's, derived from the same table the forward
	 * direction reads: a second copy of the algebra is a copy that can be
	 * edited without the other one, and the two only have to disagree once
	 * (for the `hw touch` verb, for the apitest injection cases) to look
	 * like a broken panel. */
	aster_plat_display_touch_unmap(&x, &y);

	k_mutex_lock(&pt_lock, K_FOREVER);
	pt.x = (uint16_t)x;
	pt.y = (uint16_t)y;
	pt.has_pos = true;
	if (action == ASTER_TOUCH_DOWN) {
		if (!pt.down) {
			pt.down = true;
			if (!pt.pending_down) { pt.down_x = pt.x; pt.down_y = pt.y; }
			pt.pending_down = true;
		}
		pt.pending_move = true;
	} else if (action == ASTER_TOUCH_MOVE) {
		if (pt.down) {
			pt.pending_move = true;
		}
	} else if (pt.down) {
		pt.down = false;
		pt.pending_up = true;
	}
	k_mutex_unlock(&pt_lock);
}
