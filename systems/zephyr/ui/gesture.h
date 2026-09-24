/* SPDX-License-Identifier: MIT
 * Touch gesture recognizer: press/move/release samples in, one discrete
 * gesture out. Pure -- no LVGL, no policy, no knowledge of what a gesture
 * means. The consumer (launcher.c, qpanel.c) maps ASTER_GEST_* to actions.
 *
 * Why a recognizer at all, when LVGL has an indev: the touch ring in
 * firmware/platform/esp32s3/input.c is a DESTRUCTIVE single consumer --
 * aster_plat_input_poll() clears the pending flags as it hands them over --
 * so anything that wants to see a stroke has to sit on the poll, not
 * beside it. LVGL is still fed every sample and remains the authority on
 * widgets; this layer only claims the strokes LVGL is not meant to see,
 * and the caller suppresses the corresponding click with
 * lv_indev_wait_release().
 *
 * One-sample decisions, deliberately: the shell injector (`hw touch
 * drag`, firmware/platform/esp32s3/hw_cmd.c) sends DOWN, 40 ms, ONE move,
 * 40 ms, UP. Any rule that wants "three consecutive samples in the same
 * direction" can never fire under it, so the only rule here is start
 * point -> current point.
 */
#ifndef ASTER_GESTURE_H
#define ASTER_GESTURE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	ASTER_GEST_NONE = 0,
	ASTER_GEST_PANEL_DOWN, /* pull down from the top band: toggle the panel */
	ASTER_GEST_PANEL_UP,   /* pull up from the bottom band: quit the app   */
	ASTER_GEST_PAGE_NEXT,  /* swipe left: next launcher page               */
	ASTER_GEST_PAGE_PREV,  /* swipe right: previous launcher page          */
} aster_gest_t;

/* Which gestures the context under the finger wants. A mask rather than
 * the single `allow_page` flag the plan sketched: "page" and "exit" are
 * live in *different* contexts and can be off together (the launcher with
 * the panel open), so one bit cannot express that state without turning
 * pull-up-exit on inside the launcher, where it must stay inert. */
#define ASTER_GEST_ALLOW_PANEL 0x1u /* pull-down opens/closes the panel  */
#define ASTER_GEST_ALLOW_EXIT  0x2u /* pull-up quits the foreground app  */
#define ASTER_GEST_ALLOW_PAGE  0x4u /* horizontal swipe flips a page     */

/* Thresholds in LOGICAL pixels (the space aster_plat_input_poll() reports in,
 * so a rotated panel measures the same screen the user is looking at). Named
 * here so the consumer, the shell tests and the verification sheet all quote
 * the same numbers instead of three copies of `28`. */
/* How close to an edge a pull has to START. An EDGE MARGIN, not an absolute
 * y: the band a pull-up must begin in is `screen_h - this`, which is what
 * makes it survive a change of panel geometry. It used to be the literal
 * 292 -- correct for one 320-tall panel and, on a 240-tall one, never
 * satisfiable at all: pull-up-exit simply stopped firing, with no assert, no
 * log, and nothing on the glass to say a gesture had gone missing. */
#define ASTER_GEST_EDGE_BAND   28
#define ASTER_GEST_PULL_MIN    24  /* vertical travel that makes a pull    */
#define ASTER_GEST_SWIPE_MIN   40  /* horizontal travel that makes a swipe */
/* ... and the travel along the gesture's axis must beat this percentage of
 * the travel across it. 150 = the plan's `travel > 1.5 * cross`. A ratio
 * and not an angle: integer compare, no division, no float. */
#define ASTER_GEST_DOMINANCE_PCT 150

/* Feed one sample. `pressed` is the DOWN/MOVE vs UP distinction (the
 * platform's ASTER_TOUCH_* actions collapse to exactly that bit -- a MOVE
 * with the finger up is not a thing). `screen_h` is the logical height of
 * the display the coordinates are in, which is what the bottom edge band is
 * measured from; callers pass their own geometry rather than this file
 * taking a dependency on LVGL or the devicetree, which is the property that
 * keeps the recognizer testable in isolation.
 *
 * Returns the gesture the sample decided, or ASTER_GEST_NONE. A decided
 * gesture is LATCHED: the rest of the stroke returns NONE until the release
 * clears it, which is what stops the injector's UP -- sent at the MOVE's
 * coordinates -- from being re-judged as the opposite gesture. */
aster_gest_t aster_gesture_feed(int32_t x, int32_t y, bool pressed, uint32_t allow,
			    int32_t screen_h);

/* Drop the stroke in flight, if any. Called when the context changes
 * under a finger (launcher show/hide, app stop) so the release arriving
 * after the switch is an ordinary release for the new context rather than
 * the tail of a gesture that context never saw the start of. */
void aster_gesture_reset(void);

#endif /* ASTER_GESTURE_H */
