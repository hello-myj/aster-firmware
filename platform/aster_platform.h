/* SPDX-License-Identifier: MIT
 * ASTER Platform Layer: maps Zephyr device APIs into ASTER capabilities
 * (design doc sec.5 / sec.64). Only this layer touches Zephyr.
 */
#ifndef ASTER_PLATFORM_H
#define ASTER_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <aster/display.h>
#include <aster/error.h>
#include <aster/event.h>

#define ASTER_PLAT_MAX_TOUCH 1
#define ASTER_PLAT_MAX_KEYS  8
/* terminator written into points[2] when no contact */
#define ASTER_INPUT_TERM     0xFFFFFFFFu

typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t action; /* ASTER_TOUCH_x (down/move/up) — edge-tracked by platform */
} aster_touch_evt_t;

typedef struct {
	uint32_t code;    /* raw host key code (Zephyr INPUT_KEY_*) */
	uint32_t pressed; /* 1 = down edge, 0 = up edge */
} aster_key_evt_t;

int  aster_platform_init(void);

/* display */
int  aster_plat_display_count(void);
/* Worker-thread control; same firmware can enter/leave headless mode. */
int  aster_plat_display_set_enabled(bool enabled);
int  aster_plat_display_get_info(uint32_t index, aster_display_info_t *info);
/* write w*h RGB565 pixels from native buffer to display. `pitch` is in
 * PIXELS per row, not bytes: the implementation multiplies it by 2 itself,
 * and every caller passes a width (w, a display width, a canvas width).
 * Passing bytes/row makes the staging gather step twice as far per row as
 * the buffer holds and read off its end. */
int  aster_plat_display_write_rgb565(const uint8_t *buf, uint32_t w, uint32_t h, uint32_t pitch);
/* same, at a device offset (x,y) -- for band/scrolled paints and hw tests */
int  aster_plat_display_write_at(const uint8_t *buf, uint32_t x, uint32_t y,
			       uint32_t w, uint32_t h, uint32_t pitch);
/* be_pixels_swapped: buf rows are ALREADY in panel byte order (big-endian
 * RGB565, e.g. an LVGL RGB565_SWAPPED band). At rotation 0 the platform
 * then sends buf straight to the panel, skipping the staging gather copy
 * entirely; any rotation still takes the correct double-swap gather path.
 * write_at() == write_at_ex(..., false). */
int  aster_plat_display_write_at_ex(const uint8_t *buf, uint32_t x, uint32_t y,
				  uint32_t w, uint32_t h, uint32_t pitch,
				  bool be_pixels_swapped);
/* Internal native-UI frame: persistent BE full-panel double buffers only.
 * Serialize screenshots with drawing and wait for flush before buffer reuse.
 * Not available to APPs; short-lived buffers must use write_at_ex. */
int aster_plat_display_write_persistent_ui(const uint8_t *buf,uint32_t w,uint32_t h,uint32_t pitch);
/* Full-screen flip (CONFIG_ASTER_DISP_FLIP_DBUF): byte-swap the LE RGB565
 * source into one of two PSRAM front buffers and hand it to the display
 * worker, returning before the wire finished. The caller's buffer is NOT
 * referenced by the worker, so it may be overwritten immediately -- that
 * is what lets the next frame's rasterization overlap this send. Frames
 * are tear-free because consecutive flips use different front buffers.
 * Returns ASTER_ENOTSUP when the fast path is unavailable (feature off,
 * buffers not allocated, rotation != 0, ragged pitch, or a non-screen
 * rect); callers must then fall back to the staging write path. */
int  aster_plat_display_flip_rgb565(const uint8_t *src_le, uint32_t w, uint32_t h,
				  uint32_t pitch);
/* Panel-order canvas (CONFIG_ASTER_DISP_FLIP_DBUF): the same two front
 * buffers the flip path uses, handed to the gfx engine as a draw target so
 * pixels are BORN in panel byte order and presenting needs no conversion
 * sweep at all. back() returns the buffer to rasterize into, present()
 * flushes the cache, hands it to the worker and alternates.
 * The app must repaint the entire canvas on every present: the two buffers
 * alternate, so the one being drawn into holds no previous frame.
 * ready() is false when the feature is off, buffers are missing, or the
 * panel is rotated; the runner then keeps the ordinary LE surface path. */
bool     aster_plat_display_canvas_ready(void);
uint8_t *aster_plat_display_canvas_back(void);
int      aster_plat_display_canvas_present(void);

/* App coordinate space: the panel turned portrait, i.e. (short edge, long
 * edge) -- 240x320 on this glass, the same in both orientations. App
 * surfaces are created and laid out in this space, so one app binary serves
 * a board whose panel is landscape and one whose panel is portrait without
 * knowing which it is on.
 *
 * When app space is not the panel's shape the two calls below scale the
 * frame by the largest factor that fits, centre it, and leave the rest of
 * the panel black; when it is (portrait here) they are byte-for-byte the
 * old 1:1 paths. write_app_at() takes a rect in APP coordinates -- the GFX
 * engine's dirty rect, or a whole frame -- and app_present() is the
 * whole-frame shortcut, which gets the double-buffered async flip the 1:1
 * full-screen path gets. Returns ASTER_ENOTSUP / ASTER_EINVAL on a bad rect.
 *
 * app_space() fills in the coordinate space; app_scaled() says whether the
 * platform is scaling at all (false means app space == panel space, so
 * callers can skip work that only matters under a scale). */
bool aster_plat_display_app_space(uint32_t *w, uint32_t *h);
bool aster_plat_display_app_scaled(void);
/* The inverse map, for input: x,y are panel pixels and come back as app
 * pixels. False means the point is on the letterbox bars and the caller must
 * drop the sample -- there is no app pixel under it. Always true (and the
 * coords untouched) when app space is the panel's, so the guest delivery
 * path needs no orientation test of its own. */
bool aster_plat_display_app_touch(uint32_t *x, uint32_t *y);
int  aster_plat_display_write_app_at(const uint8_t *buf, uint32_t x, uint32_t y,
				   uint32_t w, uint32_t h, uint32_t pitch);
int  aster_plat_display_app_present(const uint8_t *src_le, uint32_t w, uint32_t h,
				  uint32_t pitch);
/* What the glass is showing: the last FULL-panel frame handed to the wire,
 * in panel byte order, and invalidated by any partial update.
 *
 * Screen-grab support (ui_screenshot.c). It exists because some apps paint
 * their own LE surface and present it -- the v1 inline path (game.2048,
 * gallery) -- and therefore touch neither the gfx engine nor LVGL. Both of
 * those were asked "what is on the panel?" and both answered about
 * something else (the engine had no frame at all; LVGL was sitting on a
 * blank screen because a game app owned the display), so a screen-grab of
 * such an app was actively misleading. */
bool aster_plat_display_glass_frame(const uint8_t **buf, uint32_t *w, uint32_t *h,
				  uint32_t *pitch);
/* The window moved under the record, so what it holds describes the one the
 * glass no longer shows. Call this BEFORE repainting the new window: it
 * empties the record and sizes it to the window showing now, and the repaint
 * that follows fills it back in. Who calls it, and why not the turn itself:
 * the thread that turns the panel (a shell command, the S6 app lease) does not
 * know when the repainter runs, and blanking from there lands in the middle of
 * a frame that is already being composited -- measured, and it leaves a grab
 * showing a fraction of the picture. It is idempotent: a record that already
 * describes the current window is left untouched, so a caller that repaints
 * the same window twice loses nothing. */
void aster_plat_display_glass_rearm(void);
/* With CONFIG_ASTER_DISP_ASYNC, direct-send rects are queued to a worker
 * thread and this call returns before the wire finished. Block until the
 * queued rect is fully on the panel. LVGL wires this to flush_wait_cb;
 * every other pixel consumer (shell, colorbar, screenshots) that must
 * observe an ordered panel state calls it before touching the display.
 * No-op when the async pipeline is compiled out. */
void aster_plat_display_flush_drain(void);
/* Platform-owned logical rotation (see Kconfig ASTER_DISPLAY_ROTATION): all
 * coords above and in get_info() are LOGICAL; the port maps them to the
 * panel-native window internally. Helpers exposed for input mapping + shell: */
int  aster_plat_display_rotation_set(int deg); /* 0/90/180/270 CW */
int  aster_plat_display_rotation_get(void);
/* Ask for ASTER_ORIENT_PORTRAIT or ASTER_ORIENT_LANDSCAPE, as the logical space
 * the app / UI will be drawing into. The port answers with the panel's own
 * second window when it has one (a register write, free) and with software
 * rotation when it does not (a per-pixel transpose) -- see display.c, which
 * is where the choice and its cost are documented. Returns ASTER_ENOTSUP when
 * neither is available, so a caller always learns whether it got what it
 * asked for rather than assuming the switch happened. */
int  aster_plat_display_orientation_set(int orient);
/* The current LOGICAL orientation, or -1 with no display. Derived from the
 * same geometry get_info() reports, so the two cannot disagree. */
int  aster_plat_display_orientation_get(void);
/* raw digitizer report -> LOGICAL space, for touch. ONE call, not two: it
 * applies the window's digitizer map (the port's table of how the panel is
 * bonded, which is a property of the hardware and not derivable) and then
 * the logical rotation above, in that order. A call site must never apply
 * one half without the other -- see display.c. */
void aster_plat_display_touch_map(uint32_t *x, uint32_t *y);
/* LOGICAL -> raw digitizer report, for injecting a synthetic touch at a
 * logical point. The exact inverse of the above, derived from the same table
 * rather than written out a second time. */
void aster_plat_display_touch_unmap(uint32_t *x, uint32_t *y);
int  aster_plat_backlight_set(bool on, uint8_t level); /* level 0..255 (on only) */
int  aster_plat_display_clear(uint16_t rgb565);

/* input: pump Zephyr input events -> touch events; returns count (0..2).
 * action DOWN on first report of a point, UP when it disappears. */
int  aster_plat_input_poll(aster_touch_evt_t *evt, int max);
bool aster_plat_input_touch_available(void);

/* raw touch state for polling ABI */
void aster_plat_input_state(uint32_t *points /*4 fields per point: x,y,action,slot*/, uint32_t max);

/* inject is for debug shells to simulate a finger without a digitiser (or
 * without a hand free). x,y are LOGICAL -- what an app lays out in and what
 * a screenshot shows -- and travel the normal panel->logical path on their
 * way out, so a rotated panel needs no arithmetic at the call site.
 * action is ASTER_TOUCH_DOWN / _MOVE / _UP; MOVE and UP on a released screen
 * are dropped, exactly as the driver's would be. */
void aster_plat_input_touch_inject(uint32_t x, uint32_t y, uint32_t action);

/* keyboard / buttons: drain edge events (UP counts included; codes < 0xFFFF
 * only), report capability, snapshot currently-held codes (returns count).
 * inject is for debug shells to simulate a press without a finger. */
int  aster_plat_input_keys_poll(aster_key_evt_t *evt, int max);
bool aster_plat_input_keys_available(void);
int  aster_plat_input_keys_state(uint32_t *codes, int max);
void aster_plat_input_keys_inject(uint32_t code, bool pressed);

/* internal: called by aster_platform_init */
int aster_input_platform_init(void);

/* sensors (shell only) */
int  aster_plat_imu_read(float *ax, float *ay, float *az, float *gx, float *gy, float *gz);

/* ---- hardware geometry, asked rather than assumed ----------------------
 *
 * These four facts used to be #defines in this header: one board's camera
 * window and one board's PCM format, sitting in the contract that the
 * *port-independent* runtime is compiled against. The runtime size-checks
 * every app buffer with them, so a port whose sensor is not QVGA or whose
 * codec runs at 48 kHz would have had every aster_cam_read() / aster_audio_record()
 * rejected ASTER_EINVAL before the driver was ever entered -- a whole subsystem
 * dead on arrival, with the log naming nothing.
 *
 * The numbers are still per-board and still compile-time (the port programs
 * them into hardware registers); what changed is that they are the *port's*,
 * declared in firmware/Kconfig, and the runtime asks for them here. A port
 * answers -ENODEV only when it genuinely has no such hardware; a port that
 * has it must answer with the geometry it actually programs.
 *
 * Same rule as everywhere else in this header: the system layer never learns
 * a board's numbers, it only learns that the board has the hardware. */
int aster_plat_cam_info(uint32_t *w, uint32_t *h);
int aster_plat_audio_info(uint32_t *rate, uint32_t *channels, uint32_t *bits);

/* Bytes in one native PCM frame. Derived from the Kconfig facts rather than
 * restated, so it cannot drift from what audio.c programs into the codec --
 * and it is the one expression the port needs often enough (buffer sizing,
 * block alignment, WAV headers) that writing it out at each site would be
 * worse. Port-side only: the runtime asks aster_plat_audio_info() instead. */


/* capture a single QVGA RGB565 frame and write raw bytes to `path` (SD). */
int  aster_plat_cam_capture_rgb565(const char *path);
/* One-shot capture into a caller buffer (>= w*h*2 bytes; ask aster_plat_cam_info
 * for w/h). */
int  aster_plat_cam_grab_rgb565(uint8_t *dst, size_t dst_len);

int  aster_plat_audio_play(const uint8_t *pcm, size_t size);
int  aster_plat_audio_record(uint8_t *pcm, size_t size);
int  aster_plat_audio_beep(uint32_t duration_ms, uint32_t hz);
int  aster_plat_audio_set_volume(int percent); /* 0..100 */
int  aster_plat_audio_get_volume(void);        /* 0..100 */
int  aster_plat_audio_effect_load(const char *path);
int  aster_plat_audio_effect_play(uint32_t handle);
int  aster_plat_audio_effect_unload(uint32_t handle);
void aster_plat_audio_effect_reset(void);
/* Stream WAV/PCM/MP3 from the filesystem; no whole-file copy. */
int  aster_plat_audio_start_path(const char *path);
int  aster_plat_audio_play_path(const char *path);
int  aster_plat_audio_pause(void);
int  aster_plat_audio_resume(void);
int  aster_plat_audio_stop(void);
int  aster_plat_audio_get_state(uint32_t *state, uint32_t *total, uint32_t *done,
			     uint32_t *volume);
int  aster_plat_audio_get_clock(uint32_t *msec_done, uint32_t *msec_total);
int  aster_plat_audio_seek(uint32_t msec);
int  aster_plat_audio_get_lyrics(char *buf, size_t max);
/* True if `buf` looks like MPEG Layer III (ID3 or a frame header). */
bool aster_plat_audio_is_mp3(const uint8_t *buf, size_t len);
/* Optional probe: write codec IDs (0 if absent). Used by `hw audio info`. */
int  aster_plat_audio_probe(uint8_t *dac_id, uint8_t *adc_id);

#define ASTER_WIFI_SSID_MAX 32
#define ASTER_WIFI_AP_MAX   16

typedef struct {
	char ssid[ASTER_WIFI_SSID_MAX + 1];
	int8_t rssi;
	uint8_t channel;
	uint8_t security;
} aster_wifi_ap_t;

typedef struct {
	char ssid[ASTER_WIFI_SSID_MAX + 1];
	char ip[16];
	int8_t rssi;
	int state;
	uint8_t security;
	const char *state_txt;
	const char *sec_txt;
} aster_wifi_status_t;

enum aster_plat_wifi_state {
	ASTER_PLAT_WIFI_STATE_DISCONNECTED = 0,
	ASTER_PLAT_WIFI_STATE_DISABLED,
	ASTER_PLAT_WIFI_STATE_INACTIVE,
	ASTER_PLAT_WIFI_STATE_SCANNING,
	ASTER_PLAT_WIFI_STATE_AUTHENTICATING,
	ASTER_PLAT_WIFI_STATE_ASSOCIATING,
	ASTER_PLAT_WIFI_STATE_ASSOCIATED,
	ASTER_PLAT_WIFI_STATE_FOUR_WAY,
	ASTER_PLAT_WIFI_STATE_GROUP_HANDSHAKE,
	ASTER_PLAT_WIFI_STATE_COMPLETED,
	ASTER_PLAT_WIFI_STATE_UNKNOWN,
};

enum aster_plat_wifi_security {
	ASTER_PLAT_WIFI_SECURITY_OPEN = 0,
	ASTER_PLAT_WIFI_SECURITY_PERSONAL,
	ASTER_PLAT_WIFI_SECURITY_ENTERPRISE,
	ASTER_PLAT_WIFI_SECURITY_WEP,
	ASTER_PLAT_WIFI_SECURITY_OTHER = 255,
};

int  aster_plat_wifi_scan(aster_wifi_ap_t *out, int max, int *count);
int  aster_plat_wifi_connect(const char *ssid, const char *psk);
int  aster_plat_wifi_disconnect(void);
int  aster_plat_wifi_status(aster_wifi_status_t *st);
const char *aster_plat_wifi_sec_txt(uint8_t security);
uint32_t aster_plat_wifi_security_class(uint8_t security);
uint32_t aster_plat_wifi_state_class(int state);

/* Nonblocking Wi-Fi job interface used by SDK imports and the system UI.
 * Scan/connect operations execute on a dedicated Wi-Fi work queue. */
enum aster_plat_wifi_operation {
	ASTER_PLAT_WIFI_OP_NONE = 0,
	ASTER_PLAT_WIFI_OP_SCAN,
	ASTER_PLAT_WIFI_OP_CONNECT,
	ASTER_PLAT_WIFI_OP_DISCONNECT,
	ASTER_PLAT_WIFI_OP_RECONNECT,
	ASTER_PLAT_WIFI_OP_FORGET,
	ASTER_PLAT_WIFI_OP_HOTSPOT_START,
	ASTER_PLAT_WIFI_OP_HOTSPOT_STOP,
};
typedef struct {
	char ssid[ASTER_WIFI_SSID_MAX + 1];
	bool enabled;
	bool configured;
} aster_wifi_hotspot_status_t;
/* NULL SSID/PSK reuse the saved hotspot. An empty PSK retains its password.
 * The hotspot provides a local network at 192.168.4.1, without routing/NAT. */
int aster_plat_wifi_hotspot_start(const char *ssid, const char *psk);
int aster_plat_wifi_hotspot_stop(void);
void aster_plat_wifi_hotspot_status(aster_wifi_hotspot_status_t *status);
int  aster_plat_wifi_scan_start(void);
int  aster_plat_wifi_scan_results(aster_wifi_ap_t *out, int max, int *count);
int  aster_plat_wifi_connect_start(const char *ssid, const char *psk);
int  aster_plat_wifi_disconnect_start(void);
int  aster_plat_wifi_reconnect_start(void);
int  aster_plat_wifi_forget_start(void);
void aster_plat_wifi_operation(uint32_t *operation, uint32_t *last_operation,
			       int *last_result);

/* Called after the SD card mounts: loads the saved profile and queues the
 * boot reconnect without delaying system UI startup. */
int  aster_plat_wifi_storage_ready(void);
int  aster_plat_wifi_forget(void);

/* The SSID of the last successful join, held in RAM after load from the
 * system KV profile (0, -ENOENT if none, -EBUSY during a join). */
int  aster_plat_wifi_last_ssid(char *buf, size_t n);
/* Blocking platform operation; SDK and UI callers should queue the async
 * variant. -ENOENT when no successful profile is available. */
int  aster_plat_wifi_reconnect(void);


#endif /* ASTER_PLATFORM_H */
