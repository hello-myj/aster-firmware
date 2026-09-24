/* SPDX-License-Identifier: MIT
 * Touch gesture recognizer. See gesture.h for the contract and for why
 * the rules are one-sample.
 */
#include "gesture.h"
#include "ui_geom.h"

/* The two edge bands have to be disjoint. Where they overlap the pull-down
 * test runs first (it is the more specific claim, see the ordering in
 * aster_gesture_feed) and the pull-up silently ceases to exist -- which is the
 * same class of death the old literal 292 died of on a 240-tall screen: no
 * assert, no log, nothing on the glass. The height lives in ui_geom.h and the
 * band width lives here, so the check lives where both are visible.
 *
 * Disjointness is the only constraint worth stating: it is strictly stronger
 * than "the pull has room to travel", since a screen tall enough to separate
 * the bands is a fortiori tall enough to move ASTER_GEST_PULL_MIN inside one. */
BUILD_ASSERT(ASTER_UI_H > 2 * ASTER_GEST_EDGE_BAND,
	     "screen too short to separate the gesture edge bands: they "
	     "overlap, and the pull-up one stops being reachable");

/* The stroke in flight. One finger, one stroke: the panel's ft5336 is
 * single-touch and the platform ring tracks exactly one point, so a
 * second finger cannot start a second stroke here (it moves this one). */
static struct {
	bool     down;    /* a stroke is in flight           */
	bool     latched; /* it already produced its gesture */
	bool     top;     /* it started in the top band      */
	bool     bottom;  /* ...or in the bottom one         */
	uint32_t allow;   /* the mask captured at press      */
	int32_t  x0, y0;
} g;

void aster_gesture_reset(void)
{
	g.down = false;
	g.latched = false;
	g.top = false;
	g.bottom = false;
	g.allow = 0;
}

/* Does the travel along the gesture's axis own the stroke? Takes absolute
 * values -- the caller has already checked the direction. */
static bool dominant(int32_t travel, int32_t cross)
{
	return 100 * travel > (int32_t)ASTER_GEST_DOMINANCE_PCT * cross;
}

aster_gest_t aster_gesture_feed(int32_t x, int32_t y, bool pressed, uint32_t allow,
			    int32_t screen_h)
{
	int32_t dx, dy, adx, ady;

	if (!pressed) {
		/* Release always ends the stroke: the one after a latched
		 * gesture, the one after a stroke the injector gave no MOVE,
		 * and the stray release that arrives after the context
		 * switched under the finger. */
		aster_gesture_reset();
		return ASTER_GEST_NONE;
	}

	if (!g.down) {
		/* Press: the start point and the bands are captured NOW. A
		 * later sample cannot move the start, and the mask is frozen
		 * for the stroke so a page flip mid-drag cannot re-enable
		 * pull-up-exit halfway through. */
		g.down = true;
		g.latched = false;
		g.x0 = x;
		g.y0 = y;
		g.allow = allow;
		g.top = y <= ASTER_GEST_EDGE_BAND;
		/* Both bands measured from their own edge, so the pair stays
		 * symmetric and neither can fall off the screen. */
		g.bottom = y >= screen_h - ASTER_GEST_EDGE_BAND;
		return ASTER_GEST_NONE;
	}

	if (g.latched) {
		return ASTER_GEST_NONE; /* one gesture per stroke */
	}

	dx = x - g.x0;
	dy = y - g.y0;
	adx = dx < 0 ? -dx : dx;
	ady = dy < 0 ? -dy : dy;

	/* Edge-anchored pulls first: the bands make them the more specific
	 * claim, and the dominance test keeps them off diagonal strokes. */
	if ((g.allow & ASTER_GEST_ALLOW_PANEL) && g.top &&
	    dy >= ASTER_GEST_PULL_MIN && dominant(ady, adx)) {
		g.latched = true;
		return ASTER_GEST_PANEL_DOWN;
	}
	if ((g.allow & ASTER_GEST_ALLOW_EXIT) && g.bottom &&
	    -dy >= ASTER_GEST_PULL_MIN && dominant(ady, adx)) {
		g.latched = true;
		return ASTER_GEST_PANEL_UP;
	}
	/* Page flips start anywhere, bands included: a horizontal drag from
	 * the top strip is a swipe, not a pull, and the dominance test has
	 * already separated the two. */
	if ((g.allow & ASTER_GEST_ALLOW_PAGE) && adx >= ASTER_GEST_SWIPE_MIN &&
	    dominant(adx, ady)) {
		g.latched = true;
		return dx < 0 ? ASTER_GEST_PAGE_NEXT : ASTER_GEST_PAGE_PREV;
	}
	return ASTER_GEST_NONE;
}
