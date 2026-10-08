/* SPDX-License-Identifier: MIT
 * Quick-settings sheet: the pull-down panel over the launcher (and, once
 * S5c lands, over a running app).
 *
 * The sheet is a GUEST of whatever screen is under it. It owns no state
 * that the screen owns, it never reaches into a tree it did not build, and
 * it does not know that a "launcher" exists -- everything it needs from the
 * screen below arrives through the theme binding at the bottom of this
 * header.
 *
 * It lives on the ACTIVE SCREEN, as that screen's last two children (scrim,
 * then sheet), and NOT on lv_layer_top(). Both give the same hit-test order
 * (lv_indev_search_obj walks children in reverse) and the same draw order,
 * and BOTH are captured by `aster ui screenshot` -- that reads the glass
 * record first, and LVGL's flush_cb writes the same write_at_ex, so a
 * top-layer sheet would land there too.
 *
 * The reason is ownership on teardown: lv_obj_clean() deletes only a node's
 * own children and does not touch lv_layer_top(), so a top-layer sheet
 * survives the host rebuilding its tree and stays on the glass as a ghost
 * describing a screen that no longer exists -- and every host would have to
 * remember to close it. On the screen it dies with the screen, so
 * correctness does not depend on any call site remembering.
 *
 * The cost is that lv_obj_clean() on the screen takes the sheet with it,
 * down a path no qpanel entry point sees; the LV_EVENT_DELETE hook in
 * qpanel.c is what makes that a clean close rather than a dangling pointer.
 * Rebuild paths still call aster_qpanel_force_close() explicitly, so the
 * sheet's own teardown runs while it is still whole.
 *
 * Threading: everything here is UI thread (the one that owns LVGL). The one
 * exception is the Wi-Fi worker the sheet spawns for a reconnect, which
 * touches nothing in here except a flag.
 */
#ifndef ASTER_QPANEL_H
#define ASTER_QPANEL_H

#include <stdbool.h>
#include <stdint.h>

#include "gesture.h"

/* The sheet's colours, read fresh every time it is built -- the theme can
 * change while the sheet is closed. */
typedef struct {
	uint32_t bg;      /* the sheet itself            */
	uint32_t fg;      /* primary ink                 */
	uint32_t dim;     /* secondary ink, rules        */
	uint32_t surface; /* row/button fill             */
	uint32_t accent;  /* the slider's filled part    */
} aster_qpanel_palette_t;

/* What the host screen lets the sheet do with its theme. Five calls, all
 * optional (bind nothing and the theme row is not built):
 *
 *   count()      how many themes there are
 *   index()      the one in use now
 *   name(idx)    a display name for one
 *   apply(idx)   switch to it -- the host persists and rebuilds
 *   palette(out) the colours above, for the CURRENT theme
 *
 * apply() is expected to be asynchronous in effect: the launcher sets a
 * dirty flag and rebuilds on its next pump, which closes this sheet (see
 * aster_qpanel_force_close()). That is deliberate -- the whole screen
 * restyles, so the sheet has nothing left to describe. */
typedef struct {
	int         (*count)(void);
	int         (*index)(void);
	const char *(*name)(int idx);
	void        (*apply)(int idx);
	void        (*palette)(aster_qpanel_palette_t *out);
} aster_qpanel_theme_ops_t;

/* UI-thread return modal. Cancelling restores the saved APP frame. Only
 * take_exit_confirmed can authorize runtime teardown; opening never exits. */
bool aster_qpanel_request_exit(const char *name);
bool aster_qpanel_exit_is_open(void);
bool aster_qpanel_take_exit_confirmed(void);

void aster_qpanel_bind_theme(const aster_qpanel_theme_ops_t *ops);

/* Which screen the sheet is being raised over -- and the reason there are two
 * of them. Over the LAUNCHER, LVGL owns the glass: deleting the sheet
 * invalidates its band and LVGL repaints it from a tree that is still there,
 * so nothing has to be saved. Over an APP it does not -- the pixels under the
 * sheet came out of the guest's own present and were never in LVGL's tree, so
 * they are copied out before the sheet covers them and written back when it
 * leaves, and guest presents are held off in between
 * (aster_qpanel_app_holds_glass()). */
typedef enum {
	ASTER_QPANEL_OVER_LAUNCHER = 0,
	ASTER_QPANEL_OVER_APP,
} aster_qpanel_host_t;

/* The gesture mask for the context under the finger. Pull-down is always
 * live -- when the sheet is open it is the way out, so turning it off would
 * strand the user. Everything else is the context's own: page swipes at
 * home, pull-up-exit inside an app, and neither while the sheet is up. */
uint32_t aster_qpanel_allow(bool launcher);

/* Feed one recognised gesture. Returns true when the sheet consumed it (the
 * caller should then stop dispatching -- the launcher only cares about the
 * page gestures, an app context only about the exit one). */
bool aster_qpanel_on_gesture(aster_gest_t g, aster_qpanel_host_t host);

bool aster_qpanel_is_open(void);

/* Build the sheet (no-op when already open, or when LVGL is not up). UI
 * thread; also reached through aster_qpanel_on_gesture().
 *
 * Over an app this can REFUSE: with no whole-panel frame on the glass to save
 * (nothing presented yet, or a record that never got seeded) there is nothing
 * to put back afterwards, and a sheet that silently ate the app's screen
 * would be worse than one that does not come up. It logs the reason and
 * leaves the sheet closed. */
void aster_qpanel_open(aster_qpanel_host_t host);

/* True while a sheet is up over an APP: the guest's presents must be
 * swallowed so a frame cannot land on top of the sheet. Asked from the wasm
 * boundary (aster_native.c), which is the only place that can tell a guest's
 * present from LVGL's own flush -- both reach the wire through the same
 * write_at_ex, so this cannot be enforced one level down. */
bool aster_qpanel_app_holds_glass(void);

/* The app under the sheet is going away -- stopped, crashed, or the launcher
 * taking the screen back. Close WITHOUT restoring: writing the saved frame
 * back would paint a dead app's last picture over whatever comes next. */
void aster_qpanel_on_app_stop(void);

/* Tear the sheet down. `restore` says whether whatever it was covering has
 * to be put back on the glass: true for a user-initiated close (scrim tap,
 * ×, a second pull-down), false when the context under it is going away
 * anyway -- a screen about to be rebuilt, or an app that just stopped,
 * where restoring would paint a dead app's last frame over the launcher. */
void aster_qpanel_close(bool restore);

/* The two call sites above, spelled out. Both are no-ops when closed. */
void aster_qpanel_force_close(void);
void aster_qpanel_dismiss(void);

/* Refresh the live rows (Wi-Fi, SD) while the sheet is up. No-op when
 * closed. Call at ~1 Hz from the UI pump; this is why the sheet does not
 * need a timer of its own. */
void aster_qpanel_tick(void);

/* Restore the persisted volume onto the codec. Called once from the
 * launcher's show path -- the only place in the firmware that runs on the
 * UI thread with the SD known mounted, which is what reading kv needs. */
void aster_qpanel_restore_volume(void);

#endif /* ASTER_QPANEL_H */
