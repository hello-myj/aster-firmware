#include <zephyr/sys/util.h>
/* SPDX-License-Identifier: MIT */
#include "aster_platform.h"
#include "aster_util.h"

#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/cache.h>
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <string.h>

LOG_MODULE_REGISTER(aster_plat, LOG_LEVEL_INF);

static const struct device *disp_dev;
static atomic_t display_enabled = ATOMIC_INIT(1);
static const struct device *bl_dev;
static K_MUTEX_DEFINE(disp_lock);
static uint16_t bl_last; /* led index within the LED device (single-backlight board -> 0) */

/* The backlight is reached through the `lcd-bl` alias rather than a node label:
 * which pin drives it (GPIO, expander output, PWM channel) is a board fact,
 * and the alias is the one DT mechanism that lets a board state it without the
 * port knowing the node's name. Two ways that goes wrong, both previously
 * quiet:
 *
 *   - No alias at all: DT_ALIAS(lcd_bl) is an undefined token, so the build
 *     does break -- but as `DT_N_ALIAS_lcd_bl undeclared`, which names neither
 *     the board fact nor the file that wants it.
 *   - An alias aimed at something that is not a GPIO LED, a bare
 *     `&gpio1 10` say: it resolves to a real device. led_on() then returns
 *     -ENOTSUP, bl_dev stays non-NULL, and aster_plat_backlight_set() reports
 *     success while the glass stays dark. That is the silent-wrong-device
 *     class M0 exists to kill, and a BUILD_ASSERT is the only thing that can
 *     catch it before a human stares at a black panel.
 *
 * The alias points at the LED *child* node; the device is its gpio-leds
 * parent, which is what the port binds. */
#if IS_ENABLED(CONFIG_LED)
BUILD_ASSERT(DT_HAS_ALIAS(lcd_bl),
	     "CONFIG_LED is on, so the port reaches the backlight through "
	     "`aliases { lcd-bl = &<led child node>; }` -- the board overlay "
	     "declares no such alias");
BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_PARENT(DT_ALIAS(lcd_bl)), gpio_leds),
	     "`lcd-bl` must point at a child of a `gpio-leds` node; aimed "
	     "anywhere else it resolves to a device that is not an LED "
	     "controller, so every backlight call would report success while "
	     "the glass stays dark");
#endif

/* ---------- panel: the devicetree and the menuconfig choice must agree ----
 * One fact declared in two places, previously kept in sync by hand --
 * PORTING.md said so, and nothing checked. WHICH controller is on the glass
 * is a devicetree compatible in firmware/boards/<board>.overlay; WHICH driver the
 * port compiles in is the ASTER_ESP32S3_PANEL choice, whose `select`s pull in
 * the Zephyr driver.
 *
 * Most of this is already covered, one stage earlier and louder: swap the
 * overlay to another controller outright and no sitronix,st7789v node
 * remains, so the `select ST7789V` has an unmet dependency and Zephyr aborts
 * at the Kconfig stage naming ASTER_PANEL_ST7789V. Nothing below runs.
 *
 * What Kconfig cannot see is WHICH node is chosen. It only asks whether
 * *some* node in the tree satisfies the driver's DT dependency; the port
 * binds `DT_CHOSEN(zephyr_display)`. Leave a stale st7789v node in the tree
 * (routine during bring-up) and point the choice at a different controller,
 * and Kconfig stays silent while the port drives a panel it has no driver
 * for -- or none, since DEVICE_DT_GET_OR_NULL() returns NULL and the display
 * is dead on the bench with nothing in the log naming the cause. That case
 * is what this assert is for; it is the one verified to still be silent
 * without it.
 *
 * Deliberately ONE-directional. "A panel was picked but the chosen node is
 * not that panel" is the broken case. The converse -- ASTER_PANEL_NONE while
 * the board overlay still describes a panel -- is the headless profile
 * working as designed, because the application overlay is always applied:
 * headless carries a full panel node it never binds, which is what
 * aster_plat_display_count() reports 0 for. A bidirectional assert here fails
 * the headless build.
 *
 * Add one clause per panel in the choice above. The compat token is the
 * WHOLE compatible string with commas folded to underscores --
 * `sitronix_st7789v`, not `st7789v`. Get it wrong and DT_NODE_HAS_COMPAT is
 * simply 0 with no diagnostic, so the assert fires on a board that is wired
 * correctly; that is how the token was found, and it is the same silent-zero
 * shape this whole block exists to remove. */
#if DT_HAS_CHOSEN(zephyr_display)
BUILD_ASSERT(!IS_ENABLED(CONFIG_ASTER_PANEL_ST7789V) ||
	     DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_display), sitronix_st7789v),
	     "CONFIG_ASTER_PANEL_ST7789V selects the sitronix,st7789v driver, but "
	     "the chosen zephyr,display node is not a sitronix,st7789v: the "
	     "panel overlay and the menuconfig panel choice disagree "
	     "(firmware/boards/<board>.overlay vs the ASTER_ESP32S3_PANEL choice)");

/* A selected panel with no geometry is the same silent failure one step in.
 * The driver reads width/height straight out of the devicetree and an
 * omitted property is 0, not an error, so panel_geom() would hand every
 * consumer a 0x0 panel. The ASTER_DISP_APP_MAX check below cannot see it --
 * zero fits in 320. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_ASTER_PANEL_ST7789V) ||
	     (DT_PROP(DT_CHOSEN(zephyr_display), width) > 0 &&
	      DT_PROP(DT_CHOSEN(zephyr_display), height) > 0),
	     "the devicetree panel node declares no width/height, so the port "
	     "would see a 0x0 display");


#else
BUILD_ASSERT(!IS_ENABLED(CONFIG_ASTER_PANEL_ST7789V),
	     "CONFIG_ASTER_PANEL_ST7789V is on but the devicetree has no chosen "
	     "zephyr,display node for it to drive");
#endif

/* The format the panel itself takes. Named once and read by get_info() below
 * so the value we report and the value we check cannot come apart. */
#define ASTER_DISP_NATIVE_FORMAT ASTER_PIXEL_RGB565

/* native_format is a promise to apps -- "hand me this and you get the fast
 * path" -- so it has to be a member of the set we advertise in
 * supported_formats. Tautological while both sides are constants; it is
 * exactly the check that the drift this table removed would have needed,
 * because RGB888 was implemented in the converter and accepted by the
 * runtime while being advertised nowhere. The moment native_format becomes a
 * per-board choice (the multi-hardware direction), the tautology stops being
 * one and this starts doing real work. */
BUILD_ASSERT((ASTER_FORMATS_CONVERTIBLE & ASTER_FMT_BIT(ASTER_DISP_NATIVE_FORMAT)) != 0,
	     "ASTER_DISP_NATIVE_FORMAT is not a row of ASTER_FORMAT_TABLE, so the "
	     "port would report a native format it cannot itself produce");

/* ---------- direct-send pipeline ----------
 * One in-flight direct-send job is enough: LVGL's double-buffer protocol
 * blocks in wait_for_flushing() before it may reuse a band, so the UI
 * thread can never queue band N+1 before band N drained. The job carries
 * an already-resolved panel rect (rotation baked in by the caller's
 * thread) plus the caller's pixel pointer -- NO copy. With
 * CONFIG_ASTER_DISP_ASYNC the transfer's per-chunk wait runs as a sleep on
 * the SPI trans_done interrupt (SPI_ESP32_SLEEP_WAIT), so while the wire
 * streams the worker sleeps and the core renders. Staging (rotation /
 * ragged pitch) is NOT pipelined: those rects gather into the single
 * swap_buf, which the worker would race with the UI thread, so they drain
 * first and run synchronously. */
struct aster_disp_job {
	const uint8_t *buf;
	struct display_buffer_descriptor desc;
	uint32_t px, py;
};

/* Direct-send core: one CASET/RASET window + the packed BE pixel stream.
 * Runs on whichever thread owns the job; disp_lock serialises the panel. */
static int disp_direct_send(const struct aster_disp_job *job)
{
#if IS_ENABLED(CONFIG_ASTER_DISP_TIMING)
	uint32_t t_wr0 = k_cycle_get_32();
#endif
	k_mutex_lock(&disp_lock, K_FOREVER);
	int err = display_write(disp_dev, job->px, job->py, &job->desc, job->buf);
	k_mutex_unlock(&disp_lock);
#if IS_ENABLED(CONFIG_ASTER_DISP_TIMING)
	static uint32_t s_dtim_n, s_dtim_wr, s_dtim_bytes;
	s_dtim_n++;
	s_dtim_wr += k_cycle_get_32() - t_wr0;
	s_dtim_bytes += job->desc.buf_size;
	if (s_dtim_n >= 64) {
		uint32_t wr_us = k_cyc_to_us_floor32(s_dtim_wr);
		LOG_INF("disp tim: n=64 avg_rect=%u B direct send (no gather) write=%u us (MB/s w=%u)",
			s_dtim_bytes / 64, wr_us,
			(unsigned)((uint64_t)s_dtim_bytes * 1000000 / 1048576 / (wr_us ? wr_us : 1)));
		s_dtim_n = s_dtim_wr = s_dtim_bytes = 0;
	}
#endif
	return err;
}

#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
/* Two in-flight slots, each owning a completion signal.
 *
 * Why more than one: LVGL's double-buffer protocol blocks in
 * wait_for_flushing() before it reuses a band, so one slot is provably
 * enough for it (the UI thread cannot queue band N+1 before band N
 * drained). A full-screen flip has the same shape but a different
 * timeline: the buffer it just handed to the wire is NOT the buffer it
 * will draw into next frame, so blocking the poster on the wire throws
 * away the whole overlap. Frames are strictly serialized on the wire, so
 * "wait for MY slot's previous send" is the only ordering a poster needs
 * -- that is what the per-slot semaphore expresses.
 *
 * Slot 0 is what LVGL uses, and waiting for slot 0 before refilling it is
 * byte-for-byte the old single-slot behaviour. The flip path deliberately
 * does NOT share that slot: a full-screen flip and an LVGL band post can
 * interleave on the same thread, and sharing would make whichever came
 * second wait on the other's wire time for no reason. */
#define ASTER_DISP_JOB_SLOTS 3
#define ASTER_DISP_SHARED_SLOT 0    /* LVGL bands / generic rects */
#define ASTER_DISP_FLIP_SLOT0 1     /* flip front buffer 0 */
/* flip front buffer 1 == ASTER_DISP_FLIP_SLOT0 + 1 == slots - 1 */
struct aster_disp_slot {
	struct aster_disp_job job;
	struct k_sem free;   /* taken: a send for this slot is in flight */
};
static struct aster_disp_slot s_slots[ASTER_DISP_JOB_SLOTS];
/* Poster -> worker hand-off, one entry per armed slot. The ring can never
 * overflow: arming slot i requires s_slots[i].free, and the worker only
 * gives that back after it dequeued i, so at most ASTER_DISP_JOB_SLOTS
 * entries are live at any instant. A mutex is not needed for the same
 * reason -- only the worker advances tail, and only after it saw the
 * ready-count for the matching head publish. */
static K_SEM_DEFINE(s_job_ready, 0, ASTER_DISP_JOB_SLOTS);
static uint8_t s_armed[ASTER_DISP_JOB_SLOTS];
static volatile uint8_t s_arm_head, s_arm_tail; /* SPSC ring, poster -> worker */

static void disp_worker_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	for (;;) {
		k_sem_take(&s_job_ready, K_FOREVER);
		uint8_t slot = s_armed[s_arm_tail];

		s_arm_tail = (uint8_t)((s_arm_tail + 1) % ASTER_DISP_JOB_SLOTS);
		(void)disp_direct_send(&s_slots[slot].job); /* no error channel at wait time */
		k_sem_give(&s_slots[slot].free);
	}
}
K_THREAD_DEFINE(disp_worker, 2048, disp_worker_fn, NULL, NULL, NULL,
		CONFIG_ASTER_DISP_WORKER_PRIO, 0, 0);

static void disp_slots_init(void)
{
	for (int i = 0; i < ASTER_DISP_JOB_SLOTS; i++) {
		k_sem_init(&s_slots[i].free, 1, 1);
	}
}

/* Two-step arming: acquire the slot (blocks only until *this* slot's
 * previous send completed, so a poster never waits on the other slot's
 * wire time), fill s_slots[slot].job, then arm it. The caller must not
 * touch that job's memory again until it re-acquires the same slot. */
static void disp_slot_acquire(uint8_t slot)
{
	k_sem_take(&s_slots[slot].free, K_FOREVER);
}

static void disp_slot_arm(uint8_t slot)
{
	s_armed[s_arm_head] = slot;
	s_arm_head = (uint8_t)((s_arm_head + 1) % ASTER_DISP_JOB_SLOTS);
	k_sem_give(&s_job_ready);
}

static void disp_slot_post(uint8_t slot, const struct aster_disp_job *job)
{
	disp_slot_acquire(slot);
	s_slots[slot].job = *job;
	disp_slot_arm(slot);
}
#endif /* CONFIG_ASTER_DISP_ASYNC */

/* Block until every queued direct-send job (if any) hit the panel. Callers
 * that write the panel through any other path MUST drain first: otherwise
 * their CASET/RASET window could overtake a queued job on the wire and the
 * two rects swap screen positions. With no async pipeline every send is
 * already synchronous, so this is a no-op. */
void aster_plat_display_flush_drain(void)
{
#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
	for (int i = 0; i < ASTER_DISP_JOB_SLOTS; i++) {
		/* Re-acquiring a slot's `free` means its send finished; hand it
		 * straight back so the slot stays reusable. */
		k_sem_take(&s_slots[i].free, K_FOREVER);
		k_sem_give(&s_slots[i].free);
	}
#endif
}

int aster_plat_display_count(void)
{
	/* Menuconfig said "no panel": report zero displays even if the board
	 * devicetree still describes one. This is the third enforcement layer
	 * (install-time capability refusal, abi_min launch refusal, and here
	 * runtime ENOTSUP/ENOENT for already-installed apps). */
	return IS_ENABLED(CONFIG_ASTER_HAS_DISPLAY) && disp_dev &&
               device_is_ready(disp_dev) && atomic_get(&display_enabled) ? 1 : 0;
}

int aster_plat_display_set_enabled(bool enabled)
{
    if (enabled && (!disp_dev || !device_is_ready(disp_dev))) {
        return ASTER_ENODEV;
    }
    aster_plat_display_flush_drain();
    atomic_set(&display_enabled, enabled ? 1 : 0);
    if (bl_dev) { (void)aster_plat_backlight_set(enabled, 255); }
    return ASTER_OK;
}

/* ---------- logical rotation (software transpose in the swap band) ----------
 * Why this lives HERE: this kit's ST7789V glass physically scans 320x240
 * (proven on HW: a 240-wide portrait window overflowed RASET and left a dead
 * colour band), and Zephyr's st7789v driver programs CASET/RASET from raw
 * host coordinates with NO transpose (unlike esp-idf's draw_bitmap, which is
 * what made the vendor kit's swap_xy portrait recipe work). MADCTL cannot
 * express portrait for a host that also wants correct logical geometry, so
 * the panel stays at its native landscape window (mdac=0x60, truth in DT)
 * and ASTER owns the logical->panel mapping: apps always see get_info()'s
 * logical size, every pixel path flows through write_at() below. */
/* uint16_t, not uint8_t: 270 is a legal rotation and does not fit in 8 bits
 * (storing it as uint8_t truncated it to 14, so `hw display rot 270` used to
 * corrupt s_rot instead of rotating -- and every `case 270` below was then a
 * dead branch the compiler flagged as -Wswitch-outside-range). */
static uint16_t s_rot = CONFIG_ASTER_DISPLAY_ROTATION; /* 0/90/180/270 CW */

static void panel_geom(uint32_t *pw, uint32_t *ph)
{
	struct display_capabilities cap;
	display_get_capabilities(disp_dev, &cap);
	*pw = cap.x_resolution;
	*ph = cap.y_resolution;
}

static void logical_geom(uint32_t *lw, uint32_t *lh)
{
	uint32_t pw, ph;
	panel_geom(&pw, &ph);
	if (s_rot % 180) { /* 90/270: axes swap */
		*lw = ph;
		*lh = pw;
	} else {
		*lw = pw;
		*lh = ph;
	}
}

/* panel-native (u,v) -> LOGICAL (lx,ly). The forward pixel maps implemented
 * in write_at() are, with lw=ph, lh=pw for the quarter turns:
 *   rot0:   (u,v) = (x, y)
 *   rot90:  (u,v) = (y, lw-1-x)
 *   rot180: (u,v) = (pw-1-x, ph-1-y)
 *   rot270: (u,v) = (lh-1-y, x)
 * Their inverses (what a touch driver's panel-space report must go through
 * before apps see it): */
static void panel_to_logical(uint32_t *x, uint32_t *y)
{
	uint32_t pw, ph, lw, lh;
	if (!disp_dev || s_rot == 0) {
		return; /* headless or no-op rotation: panel space IS logical */
	}
	panel_geom(&pw, &ph);
	lw = (s_rot % 180) ? ph : pw;
	lh = (s_rot % 180) ? pw : ph;
	uint32_t u = *x, v = *y;
	switch (s_rot) {
	case 90:  *x = lw - 1 - v; *y = u;             break;
	case 180: *x = pw - 1 - u; *y = ph - 1 - v;    break;
	case 270: *x = v;          *y = lh - 1 - u;    break;
	default:  break;
	}
}

/* --- touch map: what the digitizer reports -> what the app means ------------
 *
 * With a NEUTRAL devicetree touch node (no swapped-x-y / inverted-* /
 * screen-*), what reaches the input callback is exactly what the digitizer
 * driver hands to input_touchscreen_report_pos() -- Zephyr's
 * drivers/input/input_touch.c applies those properties, and with none of them
 * set it applies nothing. For this board that call is input_ft5336.c:133
 * `input_touchscreen_report_pos(dev, col, row, ...)`, i.e. the driver hands
 * over (col, row) = (REG_P1_Y*, REG_P1_X*): x is the digitizer's 320-long
 * axis and y its 240-short one, NO MATTER what the display window is doing.
 * That mismatch with the window is the whole problem this table solves.
 *
 * Which way round the digitizer sits is fixed by how the panel is bonded and
 * cannot be derived from anything in the tree, so it is a TABLE, one row per
 * display window. A new board writes a row; it does not re-derive Zephyr's
 * calibration quirks (Zephyr's inverted-* acts on the PRE-swap value, and its
 * screen-* / inverted-* pairing assert is an equality rather than an
 * implication -- both of which the overlays' history records the hard way).
 *
 * swap / inv_x / inv_y act in that order on the incoming pair, and w / h are
 * the LOGICAL dims of the window, which is what an inversion needs. Reading
 * the row is: swap first, then invert -- so inv_x / inv_y are written in
 * terms of the POST-swap value, unlike Zephyr's properties. Keep it that way;
 * one order, stated, is the point of owning this locally.
 */
struct touch_map {
	uint8_t  swap;
	uint8_t  inv_x;
	uint8_t  inv_y;
	uint16_t w;
	uint16_t h;
};

/* Rows are indexed by display window, and the window is derivable from the
 * panel's own geometry: st7789v_get_capabilities() returns the DT width and
 * height, which is the window MADCTL actually produced (the GRAM is
 * 240RGBx320, so the MV-transposed window is the wide one). Deriving instead
 * of storing means there is no second copy of the orientation to drift --
 * S4 replaces the derivation with a settable window, the rows stay.
 *
 * Landscape's inv_y is the digitizer's bonding (glass-verified 2026-09-22 by
 * gesture bands: a physical top-edge drag must open the panel and a bottom
 * swipe must exit; a wrong axis swaps those two, which is what identified Y
 * as the mirrored one). Portrait is the driver's built-in transpose undone:
 * portrait's logical X is the digitizer's short axis, so the pair swaps. */
static const struct touch_map s_touch_maps[2] = {
	/* [0] landscape: panel wider than tall */
	{ .swap = 0, .inv_x = 0, .inv_y = 1, .w = 320, .h = 240 },
	/* [1] portrait */
	{ .swap = 1, .inv_x = 0, .inv_y = 0, .w = 240, .h = 320 },
};

/* The window's row, or NULL when there is no display to index by. Degenerate
 * panels (square, or a zero edge) cannot pick a row by comparison, and
 * guessing would silently apply the wrong digitizer map -- the failure mode
 * this whole mechanism exists to remove -- so they are reported instead. */
static const struct touch_map *touch_map_get(void)
{
	uint32_t pw, ph;

	if (!disp_dev) {
		return NULL;
	}
	panel_geom(&pw, &ph);
	if (pw == ph || pw == 0 || ph == 0) {
		LOG_WRN("touch: panel %ux%u cannot pick a digitizer map (square "
			"or degenerate); touch will not be corrected", pw, ph);
		return NULL;
	}
	return &s_touch_maps[pw > ph ? 0 : 1];
}

/* raw digitizer report -> panel-native. */
static void touch_map_apply(const struct touch_map *m, uint32_t *x, uint32_t *y)
{
	uint32_t u = *x, v = *y;

	if (!m) {
		return;
	}
	if (m->swap) {
		*x = v;
		*y = u;
		u = *x;
		v = *y;
	}
	if (m->inv_x) {
		*x = (uint32_t)m->w - 1 - u;
	}
	if (m->inv_y) {
		*y = (uint32_t)m->h - 1 - v;
	}
}

/* panel-native -> raw digitizer report: the same row, run backwards. Both
 * directions read the SAME three flags, so a row cannot describe a map whose
 * inverse disagrees with it -- which is exactly what a hand-written inverse
 * would eventually do (input.c used to carry one, and it is the kind of pair
 * where only one of the two gets edited). */
static void touch_map_unapply(const struct touch_map *m, uint32_t *x, uint32_t *y)
{
	uint32_t u = *x, v = *y;

	if (!m) {
		return;
	}
	if (m->inv_x) {
		u = (uint32_t)m->w - 1 - u;
	}
	if (m->inv_y) {
		v = (uint32_t)m->h - 1 - v;
	}
	if (m->swap) {
		*x = v;
		*y = u;
	} else {
		*x = u;
		*y = v;
	}
}

/* The platform's one entry point for "a finger, as the app should understand
 * it". Composed here rather than at the call sites so the two halves cannot
 * be applied in the wrong order or only one of them applied. */
void aster_plat_display_touch_map(uint32_t *x, uint32_t *y)
{
	touch_map_apply(touch_map_get(), x, y);
	panel_to_logical(x, y);
}

/* And its exact inverse, for injecting a synthetic touch by LOGICAL position
 * (aster_plat_input_touch_inject): the point is stored in the space a finger's
 * report occupies and then travels the normal forward path, so it cannot
 * agree with the display by accident. */
void aster_plat_display_touch_unmap(uint32_t *x, uint32_t *y)
{
	uint32_t pw, ph, lw, lh;

	/* undo panel_to_logical() first, then the digitizer map */
	if (disp_dev && s_rot != 0) {
		panel_geom(&pw, &ph);
		lw = (s_rot % 180) ? ph : pw;
		lh = (s_rot % 180) ? pw : ph;
		uint32_t a = *x, b = *y; /* logical */
		switch (s_rot) {
		/* Each case is the forward line solved for its inputs: forward
		 * 90 is (lx,ly) = (lw-1-v, u), so u = ly and v = lw-1-lx. Note
		 * which of lw/lh appears where -- they are not equal for the
		 * quarter turns, and swapping them yields a plausible-looking
		 * inverse that is off by (ph-pw) on one axis. */
		case 90:  *x = b;          *y = lw - 1 - a; break;
		case 180: *x = pw - 1 - a; *y = ph - 1 - b; break;
		case 270: *x = lh - 1 - b; *y = a;          break;
		default:  break;
		}
	}
	touch_map_unapply(touch_map_get(), x, y);
}

int aster_plat_display_rotation_set(int deg)
{
	if (deg != 0 && deg != 90 && deg != 180 && deg != 270) {
		return ASTER_EINVAL;
	}
#if CONFIG_ASTER_APP_SPACE_DIV > 1
	/* A rotated build hands the guest logical space through write_at_ex's
	 * transpose, which is the path the scaled present does not take -- it
	 * writes the panel frame directly. Rather than let app space quietly
	 * come to mean something other than what the build says it means,
	 * refuse the combination. */
	if (deg != 0) {
		LOG_ERR("software rotation unavailable with app-space scaling "
			"(ASTER_APP_SPACE_DIV=%d): rotate the panel, not the "
			"pixels", CONFIG_ASTER_APP_SPACE_DIV);
		return ASTER_ENOTSUP;
	}
#endif
	s_rot = (uint16_t)deg;
	return ASTER_OK;
}

int aster_plat_display_rotation_get(void)
{
	return s_rot;
}

/* --- orientation: which way round the LOGICAL space is ---------------------
 *
 * Two mechanisms can answer this, and only these two:
 *
 *   1. the panel's own second window (DT mdac-alt -- see the st7789v patch).
 *      The controller transposes its addressing in hardware, so this costs
 *      nothing per pixel: one register write and the glass is genuinely
 *      240x320 instead of 320x240.
 *   2. software rotation (s_rot, the ASTER_DISPLAY_ROTATION mechanism): works
 *      on any port, costs a per-pixel transpose on every flush, and exists
 *      for controllers that answer (1) with -ENOTSUP.
 *
 * (1) is tried first because it is free. The two never compose: one is what
 * the panel does, the other is what the port does to pixels on the way there,
 * and both at once would rotate twice.
 *
 * Callers do not need to know which one answered. Everything downstream reads
 * the LOGICAL geometry -- get_info(), and through panel_geom() the touch map
 * in s_touch_maps -- and both mechanisms move that by the same 90 degrees.
 */
/* Nothing to ask the panel for: it is already showing what was wanted. Not a
 * DISPLAY_ORIENTATION_ value, so it cannot be confused with one. */
#define ASTER_ORIENT_REQ_NONE 0xff

/* Defined with the rest of the screen-grab record, below. Forward declared
 * because the window switch is the one thing that invalidates the record and
 * it lives up here -- nearer the geometry it is about than the record is. */
static void glass_forget(void);
static void glass_rearm_window(void);

/* Which Zephyr orientation request expresses "I want `want`", given the window
 * on the glass now.
 *
 * Zephyr's set_orientation() is relative to the DT-declared window, so which
 * way the driver has to turn depends on whether the current window is that one
 * or its alternate -- and the alternate is the same rectangle with its edges
 * swapped, which is exactly what current_orientation distinguishes. Get this
 * backwards and the panel still switches, just to the wrong one, which is the
 * failure mode worth having no board for: the caller asked for an ASPECT, the
 * driver answers with a window, and the two are written from opposite ends.
 *
 * Pure integers on purpose, so it can be sliced and composed with the
 * driver's half of the same decision on the host. */
static uint8_t orientation_request(uint32_t cur_w, uint32_t cur_h,
				   int cur_orient, int want)
{
	bool want_wide = (want == ASTER_ORIENT_LANDSCAPE);
	bool cur_wide = (cur_w > cur_h);
	bool at_dt = (cur_orient == DISPLAY_ORIENTATION_NORMAL);
	/* If the current window is the alternate, the DT one is its transpose */
	bool dt_wide = (cur_wide == at_dt);

	if (cur_wide == want_wide) {
		return ASTER_ORIENT_REQ_NONE;
	}
	return (want_wide == dt_wide) ? DISPLAY_ORIENTATION_NORMAL
				      : DISPLAY_ORIENTATION_ROTATED_90;
}

int aster_plat_display_orientation_set(int orient)
{
	struct display_capabilities cap;
	uint32_t pw, ph;
	bool want_wide = (orient == ASTER_ORIENT_LANDSCAPE);
	uint8_t req;
	int err;

	if (orient != ASTER_ORIENT_PORTRAIT && orient != ASTER_ORIENT_LANDSCAPE) {
		return ASTER_EINVAL;
	}
	if (!disp_dev) {
		return ASTER_ENOTSUP;
	}
	panel_geom(&pw, &ph);
	if (pw == ph) {
		LOG_WRN("orientation: panel %ux%u is square; there is no "
			"portrait or landscape to ask for", pw, ph);
		return ASTER_ENOTSUP;
	}

	display_get_capabilities(disp_dev, &cap);
	req = orientation_request(cap.x_resolution, cap.y_resolution,
				  (int)cap.current_orientation, orient);
	if (req == ASTER_ORIENT_REQ_NONE) {
		return ASTER_OK; /* already showing it */
	}

	err = display_set_orientation(disp_dev, (enum display_orientation)req);
	if (err == 0) {
		/* The pixels on the glass just moved (the controller scans the
		 * same frame memory the other way round), so the screen-grab
		 * record describes a window that is no longer showing. It is
		 * dropped, not re-sized: whoever repaints the new window owns
		 * re-arming it, and that is not this thread. See
		 * glass_rearm_window(). */
		glass_forget();
		if (s_rot != 0) {
			/* Would rotate twice. Unreachable through this
			 * function -- the register path never sets s_rot -- but
			 * a port that starts with ASTER_DISPLAY_ROTATION=90 and
			 * then switches the window would land here. */
			LOG_WRN("orientation: window switched while software "
				"rotation is %u; clearing it so the two cannot "
				"compose", (unsigned)s_rot);
			s_rot = 0;
		}
		return ASTER_OK;
	}
	if (err != -ENOTSUP) {
		LOG_ERR("orientation: panel refused %d (%d)", (int)req, err);
		return ASTER_EIO;
	}

	/* No second window on this panel: turn pixels instead. The wanted space
	 * is the panel's own when the aspects already agree, its transpose
	 * otherwise. rotation_set() refuses 90 when app-space scaling is on,
	 * which is the one combination that cannot be expressed this way. */
	return aster_plat_display_rotation_set(want_wide == (pw > ph) ? 0 : 90);
}

int aster_plat_display_orientation_get(void)
{
	uint32_t lw, lh;

	if (!disp_dev) {
		return -1; /* no display: neither orientation is true */
	}
	logical_geom(&lw, &lh);
	return (lw > lh) ? ASTER_ORIENT_LANDSCAPE : ASTER_ORIENT_PORTRAIT;
}

int aster_plat_display_get_info(uint32_t index, aster_display_info_t *info)
{
	if (index != 0 || aster_plat_display_count() == 0 || !disp_dev) {
		return ASTER_ENOENT;
	}
	uint32_t lw, lh, aw, ah;
	logical_geom(&lw, &lh);
	/* Cannot fail here -- disp_dev is non-NULL, which is the only thing
	 * this refuses -- but the fallback is stated rather than assumed: a
	 * port that cannot answer reports app space as the logical size, which
	 * is exactly what a DIV=1 build means by it. */
	if (!aster_plat_display_app_space(&aw, &ah)) {
		aw = lw;
		ah = lh;
	}
	memset(info, 0, sizeof(*info));
	info->size = sizeof(*info);
	info->version = 1;
	info->width = lw; /* LOGICAL: what the glass shows */
	info->height = lh;
	/* APP space: what a surface is created in and what a touch sample is
	 * reported in. Equal to the logical size on a board whose divisor is
	 * 1; smaller on one that scales. Both are stated because "how big a
	 * picture can I show" and "what shape is my canvas" stopped being the
	 * same question the moment a board could answer them differently. */
	info->app_width = aw;
	info->app_height = ah;
	/* Panel facts, not port guesses: both are Kconfig (see the
	 * ASTER_DISP_DPI_X100 / ASTER_DISP_REFRESH_HZ help text) and default to
	 * 0 = "unknown" rather than to a plausible-looking constant. */
	info->dpi_x100 = CONFIG_ASTER_DISP_DPI_X100;
	info->native_format = ASTER_DISP_NATIVE_FORMAT;
	/* Derived from the same table the converter and the runtime gate read,
	 * so this can no longer omit a format the port accepts (RGB888) nor
	 * name one it cannot convert. */
	info->supported_formats = ASTER_FORMATS_CONVERTIBLE;
	info->refresh_rate = CONFIG_ASTER_DISP_REFRESH_HZ;
	info->orientation = (lw > lh) ? ASTER_ORIENT_LANDSCAPE : ASTER_ORIENT_PORTRAIT;
	return ASTER_OK;
}

/* Number of front buffers the flip path alternates between. Two is the
 * minimum that lets the poster hand frame N to the wire and immediately
 * start rasterizing frame N+1 without touching what the wire is reading.
 *
 * Visible outside FLIP_DBUF on purpose. app_present_full() is at file scope
 * and opens with `if (!IS_ENABLED(CONFIG_ASTER_DISP_FLIP_DBUF) || !s_front_ok
 * || !s_front[0]) return ASTER_ENOTSUP;` -- a runtime test, which needs these
 * to be declared even when the answer is a constant false. Guarding the
 * declarations instead made them vanish on a board with no double-buffer
 * while that test stayed, which is one of the two ways boards/headless.conf
 * stopped compiling. */
#define ASTER_DISP_FRONT_N 2
static uint8_t *s_front[ASTER_DISP_FRONT_N];
/* Unlike its neighbours above, this one is READ only from the flip/canvas
 * paths, which a headless build compiles out entirely -- so it needs the
 * annotation to stay quiet there rather than a guard, which would put it back
 * inside the #if this block just got lifted out of. */
static size_t front_bytes __maybe_unused;
static bool s_front_ok;
static uint8_t s_flip; /* alternates 0/1 */

/* ---------------- what the glass is showing ----------------
 * The picture on the panel, in panel byte order, for screen-grab only.
 *
 * Every write that reaches the wire funnels through here, which is what
 * makes it authoritative: apps on the v1 inline path (game.2048, gallery)
 * paint their own LE surface and present it, touching neither the gfx
 * engine nor LVGL, so neither of those can answer "what is on the panel?"
 * about them -- and the gfx engine's own frame is the APP-SIDE buffer,
 * i.e. the pixels BEFORE the present path's byte-order pass, so a grab
 * taken from it cannot see a wrong swap on the wire. The record is taken
 * after the swap, so it can (that class of bug shipped once: see the
 * SWAR probe in the flip front-buffer setup below).
 *
 * `s_glass` is a POINTER and not always a copy. A full-panel write records
 * the buffer it just handed the wire by aliasing it -- zero cost, and that
 * is the hot path: the flip and canvas presents run at ~47 fps, where a
 * 150 KB copy per frame would cost ~4.5 ms. Aliasing is only safe for a
 * buffer whose owner will not touch it again outside this file: the
 * double-buffered s_front[] and the staging bounce swap_buf.
 *
 * A partial write is exactly what breaks an alias, so it is also what pays
 * for it: the aliased frame is copied into the shadow once
 * (glass_materialize) and every later rect is composited into it. The
 * record then lives in memory nobody else can invalidate, and stays true
 * forever -- which is what partial-rect apps (the reader the engine is
 * meant to grow) need, and what the old "any band update forgets it"
 * record could not give them.
 *
 * Cost is CONFIG_ASTER_SCREENSHOT-gated and only on the partial path: one
 * row copy per dirty row, plus one 150 KB materialize when a full frame is
 * followed by a partial one. Without that option there is no shadow and a
 * partial write still forgets the record, i.e. the v1 behaviour. */
static const uint8_t *s_glass;
static uint32_t s_glass_w, s_glass_h, s_glass_pitch;

static void glass_forget(void)
{
	s_glass = NULL;
}

#if IS_ENABLED(CONFIG_ASTER_SCREENSHOT)
static uint8_t *s_glass_buf; /* owned full-panel shadow, PSRAM */
static size_t s_glass_buf_bytes;
static bool s_glass_pinned; /* s_glass aliases a buffer we do not own */

/* The record is about to be invalidated by a partial write, or a buffer is
 * being handed back out: copy the aliased frame into the shadow so it
 * survives. */
static void glass_materialize(void)
{
	if (!s_glass_pinned) {
		return;
	}
	if (!s_glass_buf) {
		glass_forget();
		return;
	}
	__ASSERT((size_t)s_glass_h * s_glass_pitch * 2u <= s_glass_buf_bytes,
		 "glass record %ux%u pitch %u overflows the %zu B shadow",
		 s_glass_w, s_glass_h, s_glass_pitch, s_glass_buf_bytes);
	memcpy(s_glass_buf, s_glass, (size_t)s_glass_h * s_glass_pitch * 2u);
	s_glass = s_glass_buf;
	s_glass_pinned = false;
}

/* A whole-panel write replaces every pixel: whatever the record held is
 * dead, so restart it on the shadow (the bands that follow fill it in). */
static void glass_rebase_full(void)
{
	if (!s_glass) {
		return;
	}
	if (s_glass_buf) {
		s_glass = s_glass_buf;
		s_glass_pinned = false;
	} else {
		glass_forget();
	}
}

/* Composite one panel-order, packed rect (pitch px per row) onto the
 * record. Every partial write goes through here. */
static void glass_blit(const uint8_t *src, uint32_t px, uint32_t py,
		       uint32_t w, uint32_t h, uint32_t pitch)
{
	if (!s_glass) {
		return;
	}
	if (!s_glass_buf) {
		glass_forget(); /* no shadow: cannot stay true */
		return;
	}
	glass_materialize();
	/* Full-width rows at the record's own stride are one contiguous run
	 * -- the LVGL band case, and every full-panel write. */
	if (px == 0 && pitch == s_glass_pitch && w == s_glass_pitch &&
	    py + h <= s_glass_h) {
		memcpy(s_glass_buf + (size_t)py * s_glass_pitch * 2u, src,
		       (size_t)w * h * 2u);
		return;
	}
	for (uint32_t j = 0; j < h; j++) {
		memcpy(s_glass_buf + ((size_t)(py + j) * s_glass_pitch + px) * 2u,
		       src + (size_t)j * pitch * 2u, (size_t)w * 2u);
	}
}

/* Single entry point for every write that reaches the wire, so a new write
 * path cannot silently stop maintaining the record. `whole` means the rect
 * covers the panel. */
static void glass_track(const uint8_t *src, uint32_t px, uint32_t py,
			uint32_t w, uint32_t h, uint32_t pitch, bool whole)
{
	if (!s_glass) {
		return;
	}
	if (whole) {
		glass_rebase_full();
	}
	glass_blit(src, px, py, w, h, pitch);
}

/* Record a WHOLE panel frame, panel byte order already applied. `alias_ok`
 * means the caller's buffer stays untouched until the next write through
 * write_at_ex(), so the record may point at it instead of copying it. */
static void glass_note_full(const uint8_t *buf, uint32_t w, uint32_t h,
			    uint32_t pitch, bool alias_ok)
{
	if (alias_ok) {
		s_glass = buf;
		s_glass_pinned = true;
	} else if (s_glass_buf && pitch == w &&
		   (size_t)w * h * 2u <= s_glass_buf_bytes) {
		/* Someone else owns the buffer and may recycle it (an LVGL
		 * band, an app surface): take the copy. */
		memcpy(s_glass_buf, buf, (size_t)w * h * 2u);
		s_glass = s_glass_buf;
		s_glass_pinned = false;
	} else {
		glass_forget();
		return;
	}
	s_glass_w = w;
	s_glass_h = h;
	s_glass_pitch = pitch;
}

/* Empty and re-size the record to the window showing now, so it can be filled
 * back in by the repaint that follows. Called by whoever is ABOUT TO REPAINT
 * the new window (aster_plat_display_glass_rearm(), driven from the UI's window
 * resync), never by the thread that turned the panel.
 *
 * Why the split matters. A MADCTL write does not move pixels, it changes how
 * the controller SCANS frame memory, so the record describes the old window
 * the moment the panel turns -- and re-deriving what is on the glass would
 * mean reimplementing the controller's MV/ML/MH permutation, where a wrong
 * guess produces a plausible-looking wrong picture instead of an obvious empty
 * one. So it is emptied rather than transposed. That much was always here.
 *
 * What was wrong was the WHEN. Blanking used to happen inside the turn, on the
 * calling (shell) thread, while the UI thread was already mid-repaint of the
 * new window: measured on the reference board, a hand-typed `hw display orient`
 * to portrait emitted two bands -- 240x213+0+0 and 240x107+0+213 -- and the
 * blanking landed BETWEEN them, so band 1 was composited into the record and
 * then wiped, and the grab showed band 2 alone: a few rows of real picture
 * floating over black, i.e. a CROP that looks like an image. Deferring the
 * blank to the repainting thread puts it strictly before the first band of the
 * new window instead of somewhere inside them.
 *
 * The `if` is what keeps the deferral from eating an app's own frame on the
 * way past: a partial-rect app (the reader) fills the record itself through
 * glass_note_full()/glass_blit() and never asks the UI to resync, but the UI's
 * pump does resync the window it is not drawing -- and if the record already
 * describes the current window there is nothing stale about it, so it is left
 * alone.
 *
 * Without any of this the record kept the geometry of the window that first
 * filled it: measured on the reference board after an app that declares
 * portrait ran and exited, the record still said 240x320 while the UI was
 * drawing 320x240 into it, so every band was blitted at the wrong stride --
 * the grab showed a coherent-looking picture that was a CROP of the real one. */
static void glass_rearm_window(void)
{
	uint32_t pw, ph;

	if (!s_glass_buf) {
		return; /* no shadow: a record cannot exist at all */
	}
	panel_geom(&pw, &ph);
	if (s_glass && s_glass_w == pw && s_glass_h == ph) {
		return; /* already describes this window -- do not eat it */
	}
	if ((size_t)pw * ph * 2u > s_glass_buf_bytes) {
		/* One too small for the new window: the record cannot stay true
		 * and says so by going away. */
		glass_forget();
		return;
	}
	memset(s_glass_buf, 0, (size_t)pw * ph * 2u);
	s_glass = s_glass_buf;
	s_glass_pinned = false;
	s_glass_w = pw;
	s_glass_h = ph;
	s_glass_pitch = pw;
	/* INF, not DBG: a stale record is invisible to the eye -- the grab it
	 * produces looks like a coherent picture, just the wrong one -- so the
	 * only cheap evidence that this ran is the line itself. Once per turn,
	 * on a path that already logs the turn. */
	LOG_INF("screen-grab record: window is now %ux%u, awaiting repaint",
		pw, ph);
}

#else /* !CONFIG_ASTER_SCREENSHOT */

static inline void glass_rebase_full(void)
{
	glass_forget();
}

/* Nothing to re-arm: without a shadow the record only ever aliased a
 * caller-owned whole frame, and the turn already forgot it. */
static inline void glass_rearm_window(void)
{
}

static inline void glass_track(const uint8_t *src, uint32_t px, uint32_t py,
			       uint32_t w, uint32_t h, uint32_t pitch, bool whole)
{
	ARG_UNUSED(src);
	ARG_UNUSED(px);
	ARG_UNUSED(py);
	ARG_UNUSED(w);
	ARG_UNUSED(h);
	ARG_UNUSED(pitch);
	ARG_UNUSED(whole);
	glass_forget();
}

static inline void glass_note_full(const uint8_t *buf, uint32_t w, uint32_t h,
				   uint32_t pitch, bool alias_ok)
{
	ARG_UNUSED(alias_ok);
	s_glass = buf;
	s_glass_w = w;
	s_glass_h = h;
	s_glass_pitch = pitch;
}
#endif /* CONFIG_ASTER_SCREENSHOT */

void aster_plat_display_glass_rearm(void)
{
	glass_rearm_window();
}

bool aster_plat_display_glass_frame(const uint8_t **buf, uint32_t *w, uint32_t *h,
				  uint32_t *pitch)
{
	if (!s_glass) {
		return false;
	}
	*buf = s_glass;
	*w = s_glass_w;
	*h = s_glass_h;
	*pitch = s_glass_pitch;
	return true;
}

/* State for the app coordinate space, declared here rather than beside its
 * code further down because aster_plat_display_canvas_ready() -- the first
 * user -- sits above it. See the app-space block below for what it all
 * means.
 *
 * OUTSIDE the FLIP_DBUF guard that follows, deliberately. The consumers
 * (aster_plat_display_app_geom_init / _present_full / _present_rect) are at
 * file scope because mapping app space onto the panel does not depend on how
 * a frame reaches the wire. Keeping these declarations inside the guard
 * compiled on the reference board -- where FLIP_DBUF is y -- and broke every
 * profile with no panel: boards/headless.conf selects ASTER_PANEL_NONE, so
 * ASTER_HAS_DISPLAY is n, so FLIP_DBUF's `depends on` fails, so the
 * declarations vanished while their uses stayed. One array below really does
 * need FLIP_DBUF, so it stays inside. */

/* True when there is a scaling step between the guest's pixels and the glass.
 * Constant false on a DIV=1 build -- where the whole mechanism below is
 * compiled out and app space IS the logical screen -- expressed as a macro so
 * the reader of aster_plat_display_canvas_ready() sees the same condition on
 * every build rather than a variable that happens never to be set. */
#if CONFIG_ASTER_APP_SPACE_DIV > 1
static bool s_app_scaled;
#else
#define s_app_scaled (false)
#endif

/* The app-space index tables were sized by a hard-coded 320 here, and the
 * assertion below it was a plain ceiling: "your panel must not be bigger than
 * this". Both are gone. The map from app pixels to glass pixels is affine
 * (dest = src * DIV) precisely because the divisor is required to divide both
 * panel edges, so there is no table to size and no per-pixel map to build --
 * see the app-space block further down.
 *
 * What remains is the capacity declaration, and it earns its keep only when
 * there is something to put in it: the fallback scanline in app_present_rect,
 * which is the one app-space-derived buffer in the port. On a DIV=1 build
 * nothing here exists at all, so neither does the assertion. Both halves are
 * checked against the devicetree rather than trusted:
 *   - the divisor must divide both panel edges, or the nearest-neighbour map
 *     would be non-uniform and the touch inverse ambiguous;
 *   - the resulting app space must fit ASTER_APP_SPACE_MAX, or the fallback
 *     scanline overflows.
 * A board that gets either wrong fails to compile, naming the property. */
#if CONFIG_ASTER_APP_SPACE_DIV > 1
BUILD_ASSERT(DT_PROP(DT_CHOSEN(zephyr_display), width) % CONFIG_ASTER_APP_SPACE_DIV == 0 &&
	     DT_PROP(DT_CHOSEN(zephyr_display), height) % CONFIG_ASTER_APP_SPACE_DIV == 0,
	     "ASTER_APP_SPACE_DIV must divide both panel edges: a fractional "
	     "divisor makes the pixel map non-uniform and the touch inverse "
	     "ambiguous");
BUILD_ASSERT(DT_PROP(DT_CHOSEN(zephyr_display), width) <=
		     CONFIG_ASTER_APP_SPACE_MAX * CONFIG_ASTER_APP_SPACE_DIV &&
	     DT_PROP(DT_CHOSEN(zephyr_display), height) <=
		     CONFIG_ASTER_APP_SPACE_MAX * CONFIG_ASTER_APP_SPACE_DIV,
	     "app space (panel / ASTER_APP_SPACE_DIV) exceeds "
	     "ASTER_APP_SPACE_MAX, which bounds the port's app-space scratch");
#endif

#if IS_ENABLED(CONFIG_ASTER_DISP_FLIP_DBUF)

/* ---------------- panel-order canvas (engine fast path) ----------------
 *
 * aster_plat_display_flip_rgb565() above still pays for the byte-order pass:
 * the app's surface is plain LE RGB565 and the glass wants each 16-bit word
 * the other way round, so every presented frame costs a 307 KB PSRAM->PSRAM
 * sweep (~7 ms measured, against 33 MB/s of PSRAM store bandwidth).
 *
 * The canvas API removes that sweep by never producing LE pixels in the
 * first place: the gfx engine rasterizes straight into one of the two front
 * buffers in PANEL byte order (the swizzle rides along in the store, for
 * free), so presenting is just "hand this buffer to the wire and alternate".
 *
 * Contract, enforced by the runner in aster_native.c: the app must redraw the
 * whole frame every present. Alternating two buffers only works if each
 * frame rewrites every pixel -- there is no previous-frame content to fall
 * back on in the buffer being drawn into. Apps that only repaint a dirty
 * rect must keep using an ordinary LE surface.
 */
bool aster_plat_display_canvas_ready(void);
uint8_t *aster_plat_display_canvas_back(void);
int aster_plat_display_canvas_present(void);

bool aster_plat_display_canvas_ready(void)
{
	uint32_t pw, ph, lw, lh;

	if (!disp_dev || !s_front_ok || s_rot != 0 || s_app_scaled) {
		return false;
	}
	panel_geom(&pw, &ph);
	logical_geom(&lw, &lh);
	/* The buffers are panel-sized and handed to the wire untransposed, so
	 * only an unrotated panel can use them directly. s_app_scaled is the
	 * second way that fails: when the app's coordinate space is not the
	 * panel's, the engine would be rasterizing app-space pixels into a
	 * panel-space buffer, and the present that followed would scale them
	 * a second time. The app gets an ordinary LE surface instead, which is
	 * always correct -- just not free. */
	return pw == lw && ph == lh && (size_t)lw * lh * 2u <= front_bytes;
}

uint8_t *aster_plat_display_canvas_back(void)
{
	return aster_plat_display_canvas_ready() ? s_front[s_flip] : NULL;
}

int aster_plat_display_canvas_present(void)
{
	if (!aster_plat_display_canvas_ready()) {
		return ASTER_ENOTSUP;
	}
	uint8_t fi = s_flip;
	uint8_t slot = (uint8_t)(ASTER_DISP_FLIP_SLOT0 + fi);
	uint32_t pw, ph;

	panel_geom(&pw, &ph);
#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
	/* Wait for this slot's previous send before the engine draws into the
	 * buffer again -- i.e. acquire happens one frame ahead of the draw, so
	 * the caller never stalls right after rendering. The other slot keeps
	 * streaming meanwhile; it is reading the other buffer. */
	disp_slot_acquire(slot);
#endif
	/* The engine's stores left panel-order bytes dirty in the write-back
	 * D-cache; the DMA reads PSRAM, so push them out. This is the only
	 * per-frame cost left on the present path. */
	sys_cache_data_flush_range(s_front[fi], (size_t)pw * ph * 2u);
	/* Alias: this front buffer is ours, and stays untouched until the
	 * engine draws the frame two presents from now -- by which time a
	 * newer cut() has re-pointed the record. */
	glass_note_full(s_front[fi], pw, ph, pw, true);

#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
	s_slots[slot].job = (struct aster_disp_job){
		.buf = s_front[fi],
		.desc = {
			.buf_size = (size_t)pw * ph * 2u,
			.width = pw,
			.height = ph,
			.pitch = pw,
		},
		.px = 0,
		.py = 0,
	};
	disp_slot_arm(slot);
#else
	if (disp_direct_send(&(struct aster_disp_job){
			.buf = s_front[fi],
			.desc = {
				.buf_size = (size_t)pw * ph * 2u,
				.width = pw,
				.height = ph,
				.pitch = pw,
			},
		})) {
		return ASTER_EIO;
	}
#endif
	s_flip ^= 1u;
	return ASTER_OK;
}

int aster_plat_display_flip_rgb565(const uint8_t *src_le, uint32_t w, uint32_t h,
				 uint32_t pitch)
{
    if (!aster_plat_display_count()) return ASTER_ENODEV;
	uint32_t pw, ph, lw, lh;

	if (!disp_dev || !s_front_ok) {
		return ASTER_ENOTSUP;
	}
	/* rot0 only: the copy below is a straight byte swap with no
	 * transpose, and the front buffers are panel-sized. A rotated panel
	 * falls back to the caller's synchronous staging path. */
	if (s_rot != 0 || !src_le || !w || !h || pitch != w) {
		return ASTER_ENOTSUP;
	}
	panel_geom(&pw, &ph);
	logical_geom(&lw, &lh);
	if (w != lw || h != lh || (size_t)w * h * 2u > front_bytes) {
		return ASTER_ENOTSUP;
	}

	uint8_t fi = s_flip;                                  /* front buffer 0/1 */
	uint8_t slot = (uint8_t)(ASTER_DISP_FLIP_SLOT0 + fi);    /* job slot */
	uint16_t *dst = (uint16_t *)s_front[fi];

#if IS_ENABLED(CONFIG_ASTER_DISP_TIMING)
	uint32_t t_acq0 = k_cycle_get_32();
	uint32_t acc_acq;
#endif
	/* Wait for THIS slot's previous send. The other slot's send may still
	 * be streaming -- that is the whole point: it is reading a different
	 * buffer, so it cannot tear what we are about to write. */
#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
	disp_slot_acquire(slot);
#endif
#if IS_ENABLED(CONFIG_ASTER_DISP_TIMING)
	acc_acq = k_cycle_get_32() - t_acq0;
	uint32_t t_copy0 = k_cycle_get_32();
#endif
	/* LE -> panel byte order. The source is the app's authoritative back
	 * buffer (app wasm linear memory, PSRAM); the destination is PSRAM
	 * too. Byte-swap BOTH 16-bit lanes of a 32-bit load at once, i.e.
	 * two pixels per iteration.
	 *
	 * The swap is the SWAR mask pair, NOT `(v << 16) | (v >> 16)`: that
	 * rotate exchanges the two half-words (it reorders the pixels) and
	 * leaves each pixel's own bytes alone. It looks like a byte swap and
	 * is not one -- it shipped a frame whose pixels came out as
	 * 0xE007/0x00F8 where 0xF800/0x07E0 was meant.
	 *
	 * It must agree with the scalar `sys_cpu_to_be16()` used for the odd
	 * tail below and everywhere else in this file: the panel wants each
	 * pixel's high byte first, so the u16 stored in memory has to be the
	 * byte-reversed pixel value. */
	{
		const uint32_t *src = (const uint32_t *)src_le;
		uint32_t *dst32 = (uint32_t *)dst;
		size_t n = (size_t)w * h;

		for (size_t i = 0; i < n / 2u; i++) {
			uint32_t v = src[i];

			dst32[i] = ((v & 0x00FF00FFu) << 8) |
				   ((v & 0xFF00FF00u) >> 8);
		}
		if (n & 1u) { /* panel is 240x320 = even, but keep it total */
			dst[n - 1u] = sys_cpu_to_be16(((const uint16_t *)src_le)[n - 1u]);
		}
	}
	/* DMA reads s_front[fi]; the swapped bytes are dirty in the
	 * write-back D-cache and must be pushed out first. */
	sys_cache_data_flush_range(s_front[fi], (size_t)w * h * 2u);
	/* The entire panel, in panel byte order, is now in this buffer: that
	 * is literally what the glass shows, so a screen-grab of a v1-inline
	 * app can be served from here. Aliased, not copied -- this is the
	 * 47 fps path (see the glass record above). */
	glass_note_full(s_front[fi], w, h, w, true);

#if IS_ENABLED(CONFIG_ASTER_DISP_TIMING)
	/* Flip-path split. Without this the whole flip showed up as one lump
	 * inside the engine's flush time, which is exactly how the copy got
	 * misread as wire time. `acquire` is the slot wait (should be ~0 when
	 * the wire is faster than the frame), `copy` is the byte-order pass,
	 * `sync` is the cache push-out the DMA needs. */
	{
		static uint32_t s_fn, s_facq, s_fcopy;

		s_fn++;
		s_facq += acc_acq;
		s_fcopy += k_cycle_get_32() - t_copy0;
		if (s_fn >= 64u) {
			LOG_INF("flip tim: acquire=%u us copy+sync=%u us",
				(unsigned)(k_cyc_to_us_floor32(s_facq) / 64u),
				(unsigned)(k_cyc_to_us_floor32(s_fcopy) / 64u));
			s_fn = s_facq = s_fcopy = 0;
		}
	}
#endif

#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
	s_slots[slot].job = (struct aster_disp_job){
		.buf = s_front[fi],
		.desc = {
			.buf_size = (size_t)w * h * 2u,
			.width = w,
			.height = h,
			.pitch = w,
		},
		.px = 0,
		.py = 0,
	};
	disp_slot_arm(slot);
#else
	/* No worker thread: send synchronously through the direct path. */
	if (disp_direct_send(&(struct aster_disp_job){
			.buf = s_front[fi],
			.desc = {
				.buf_size = (size_t)w * h * 2u,
				.width = w,
				.height = h,
				.pitch = w,
			},
		})) {
		return ASTER_EIO;
	}
#endif
	s_flip ^= 1u;
	return ASTER_OK;
}

#else /* !CONFIG_ASTER_DISP_FLIP_DBUF */

/* No double-buffered front to hand out. The prototypes live in
 * aster_platform.h and the core calls them unconditionally, so they have to
 * exist in every profile -- display.c is still built when the menu turns the
 * panel off (it answers ASTER_ENOTSUP at runtime, see aster_plat_display_has).
 * Without these three the headless profile fails to link, not to compile. */
bool aster_plat_display_canvas_ready(void)
{
	return false;
}

uint8_t *aster_plat_display_canvas_back(void)
{
	return NULL;
}

int aster_plat_display_canvas_present(void)
{
	return ASTER_ENOTSUP;
}

#endif /* CONFIG_ASTER_DISP_FLIP_DBUF */

/* This module sits on an 8-bit DBI SPI link that streams the framebuffer
 * byte-for-byte; ST7789V latches each pixel MSB-first. Little-endian RGB565
 * (the ASTER ABI promise: R in bits 15..11, LE u16 in memory) therefore hits
 * the glass byte-swapped -- verified on HW: filling 0xF800 pure RED shows
 * BLUE until both bytes trade places. The swap belongs HERE, in the single
 * choke point every pixel path flows through (surfaces, colorbar, fills),
 * so apps and the runtime keep speaking plain LE RGB565.
 *
 * Staging never mutates the caller's buffer (LVGL PARTIAL bands are reused
 * immediately after flush). Two budgets:
 *
 *   CONFIG_ASTER_DISP_PSRAM_STAGE: one full-panel PSRAM buffer (pw*ph*2), a
 *   dirty rect becomes ONE display_write() / CASET+RASET window. The stock
 *   Zephyr SPI driver k_malloc+memcpy()s every 4092-byte chunk through an
 *   internal-RAM bounce before GDMA sends it (correct, ~26.5 fps). GDMA
 *   reading PSRAM directly banded on this glass 2026-09-09 -- root cause
 *   MSPI bus contention under load (docs/verification.md, 2026-09-10);
 *   no west-tree patch is used or needed.
 *
 *   else CONFIG_ASTER_DISP_BOUNCE_KB: internal-DRAM multi-row bands. Rows per
 *   SPI window = budget / rect stride (narrow rects buy more rows).
 *
 * CS-on-I2C is disarmed after the first DISPON
 * (CONFIG_ST7789V_HOLD_CS_AFTER_INIT); DTS mipi-hold-cs is not used because
 * this panel's init sequence needs CS edges. */
#define ASTER_DISP_MAX_ROW_PX 320u
#if IS_ENABLED(CONFIG_ASTER_DISP_PSRAM_STAGE)
static uint8_t *swap_buf;
static size_t swap_buf_bytes;
#else
#define ASTER_DISP_BOUNCE_BYTES (CONFIG_ASTER_DISP_BOUNCE_KB * 1024u)
static uint8_t swap_buf_storage[ASTER_DISP_BOUNCE_BYTES] __attribute__((aligned(32)));
static uint8_t *const swap_buf = swap_buf_storage;
static const size_t swap_buf_bytes = ASTER_DISP_BOUNCE_BYTES;
#endif

/* ---------------- app coordinate space (density-scaled present) ----------------
 *
 * An app surface is not the panel. A board whose glass has a different pixel
 * count from the one an app was authored for still runs that app: the guest
 * draws in a space that suits it, and the port puts that space on the wire.
 *
 * That space is the LOGICAL screen divided by ASTER_APP_SPACE_DIV on both axes.
 * On the reference board the divisor is 1 and this entire block does not
 * exist -- app space IS the logical screen, write_app_at() forwards to
 * write_at_ex(), and a present is the same 1:1 path it always was. Not a
 * table, not a branch, not a byte.
 *
 * On a bigger panel the divisor is what keeps an app's proportions and its
 * touch-target sizes, and it keeps the cost of a present proportional to the
 * APP's raster area rather than to the glass. The direction matters and is
 * deliberate: app space is never larger than the panel. The opposite (a
 * canvas bigger than the glass, scaled down) multiplies the same app's raster
 * area by the square of the factor and lands that cost on whichever board is
 * slowest.
 *
 * DIV must divide both panel edges, and the assertion at the top of this file
 * enforces it. That requirement is what makes the rest cheap, because
 * dest = src * DIV is then affine:
 *
 *   - there is no index table. A gather reads src = dest / DIV, and the
 *     divisor is a compile-time constant, so the compiler emits a
 *     multiply-shift -- cheaper than the table load it replaces. The earlier
 *     design built dest->src tables at init because its factor was a RATIO,
 *     which has to be computed and can be non-integral.
 *   - there are no letterbox bars. The app space covers the glass exactly, so
 *     there is no margin to paint, and none of the "who last wrote the
 *     margins" bookkeeping that went with one.
 *
 * What is left is a nearest-neighbour replicate: the same class of work the
 * 1:1 path already does (one byte-order swap per pixel), plus a row repeat. It
 * is deliberately not the per-frame synth scaling docs/multi-resolution-
 * design.md rules out -- no blending, no resampling kernel, no interpolation.
 *
 * The engine's panel-order canvas (aster_plat_display_canvas_ready) is refused
 * while this is on: the engine would rasterize app-space pixels into a
 * panel-sized buffer and the present that followed would scale them a second
 * time. The app gets an ordinary LE surface instead -- always correct, just
 * not free.
 *
 * Fonts scale whole, with the rest of the frame. At DIV=2 a 20 px bitmap font
 * lands as 2x2 blocks: legible, consistent and visibly coarse. Baking a
 * second font per density is the real fix and is not done here; the tradeoff
 * is recorded rather than papered over.
 */
#if CONFIG_ASTER_APP_SPACE_DIV > 1

/* The one app-space-derived buffer in the port: the fallback scanline used
 * when the staging bounce could not be allocated. A full logical row is app
 * space times the divisor, which is exactly what the capacity assertion at
 * the top of this file bounds. */
#define ASTER_APP_ROW_PX (CONFIG_ASTER_APP_SPACE_MAX * CONFIG_ASTER_APP_SPACE_DIV)

/* App space, in app pixels. Integer division of the logical screen; the
 * product is exactly the logical size back again, which is what makes the
 * margins disappear. */
static void app_geom(uint32_t *aw, uint32_t *ah)
{
	uint32_t lw, lh;

	logical_geom(&lw, &lh);
	*aw = lw / CONFIG_ASTER_APP_SPACE_DIV;
	*ah = lh / CONFIG_ASTER_APP_SPACE_DIV;
}

static void app_geom_init(void)
{
	uint32_t aw, ah, lw, lh;

	app_geom(&aw, &ah);
	logical_geom(&lw, &lh);
	/* Scaling and software rotation cannot both be on. The 1:1 path
	 * transposes through write_at_ex; this one writes the panel frame
	 * directly and has no transpose to offer, so scaling on top of a
	 * software rotation would ask for a map onto axes nothing rotates
	 * back. rotation_set() refuses that combination, but s_rot can also be
	 * non-zero simply because the build shipped that way
	 * (CONFIG_ASTER_DISPLAY_ROTATION), so it is re-checked here rather than
	 * assumed. */
	s_app_scaled = (s_rot == 0) && (aw * CONFIG_ASTER_APP_SPACE_DIV == lw) &&
		       (ah * CONFIG_ASTER_APP_SPACE_DIV == lh);
	if (!s_app_scaled) {
		LOG_WRN("app-space scaling off: rot %d, app %ux%u x %d does "
			"not cover logical %ux%u; presents stay 1:1",
			(int)s_rot, (unsigned)aw, (unsigned)ah,
			CONFIG_ASTER_APP_SPACE_DIV, (unsigned)lw, (unsigned)lh);
		return;
	}
	LOG_INF("app space: %ux%u (logical %ux%u / %d)",
		(unsigned)aw, (unsigned)ah, (unsigned)lw, (unsigned)lh,
		CONFIG_ASTER_APP_SPACE_DIV);
}

/* Scale the whole app frame into a front buffer and hand it to the wire.
 *
 * Same double-buffered flip the 1:1 path uses (see
 * aster_plat_display_flip_rgb565): the caller's buffer is not referenced by the
 * worker, so the next frame's rasterization overlaps this send. The
 * difference is that the byte-order pass and the scale share one store: an
 * app pixel is read and swapped once, then written DIV times across and DIV
 * times down.
 *
 * Source-driven rather than destination-driven because the map is affine --
 * the destination index is a product, not a lookup, so walking the source is
 * the cheapest way to visit every destination exactly once. */
static int app_present_full(const uint8_t *src_le, uint32_t aw, uint32_t ah,
			    uint32_t pitch)
{
	if (!IS_ENABLED(CONFIG_ASTER_DISP_FLIP_DBUF) || !s_front_ok || !s_front[0]) {
		return ASTER_ENOTSUP;
	}
	uint32_t pw, ph;
	uint8_t fi = s_flip;

	/* panel_geom, not logical: these are the front buffers, and they are
	 * panel-sized. Equal to the logical shape whenever this path runs --
	 * app_geom_init() declines to scale on a rotated panel -- so the
	 * replicate below and the buffer being filled agree by construction. */
	panel_geom(&pw, &ph);
#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
	/* Acquired BEFORE the gather, as in flip_rgb565() and unlike
	 * canvas_present() -- this is the function that writes the front
	 * buffer, so waiting for this slot's previous send has to come first,
	 * not after the pixels are already in flight. */
	uint8_t slot = (uint8_t)(ASTER_DISP_FLIP_SLOT0 + fi);

	disp_slot_acquire(slot);
#endif
	uint16_t *dst = (uint16_t *)s_front[fi];

	for (uint32_t sy = 0; sy < ah; sy++) {
		const uint16_t *srow =
			(const uint16_t *)(src_le + (size_t)sy * pitch * 2u);
		uint16_t *drow = dst + (size_t)sy * CONFIG_ASTER_APP_SPACE_DIV * pw;

		for (uint32_t r = 0; r < CONFIG_ASTER_APP_SPACE_DIV; r++) {
			for (uint32_t dx = 0; dx < aw; dx++) {
				const uint16_t v = sys_cpu_to_be16(srow[dx]);

				for (uint32_t k = 0; k < CONFIG_ASTER_APP_SPACE_DIV;
				     k++) {
					drow[dx * CONFIG_ASTER_APP_SPACE_DIV + k] = v;
				}
			}
			drow += pw;
		}
	}
	sys_cache_data_flush_range(s_front[fi], (size_t)pw * ph * 2u);
	glass_note_full(s_front[fi], pw, ph, pw, true);
#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
	s_slots[slot].job = (struct aster_disp_job){
		.buf = s_front[fi],
		.desc = {
			.buf_size = (size_t)pw * ph * 2u,
			.width = pw,
			.height = ph,
			.pitch = pw,
		},
		.px = 0,
		.py = 0,
	};
	disp_slot_arm(slot);
#else
	if (disp_direct_send(&(struct aster_disp_job){
			.buf = s_front[fi],
			.desc = {
				.buf_size = (size_t)pw * ph * 2u,
				.width = pw,
				.height = ph,
				.pitch = pw,
			},
		})) {
		return ASTER_EIO;
	}
#endif
	/* The whole panel went out. */
	s_flip ^= 1u;
	return ASTER_OK;
}

/* A sub-rect of the app frame: scale the rows it covers into the bounce
 * buffer and send them. Bands, not one row per window, because the scaled row
 * is narrow and a CASET/RASET pair per row would be most of the cost. Each
 * band is drained before the next is built -- the bounce is reused, and the
 * async slot would otherwise still be reading it.
 *
 * The rect arrives in APP pixels and is expanded by the divisor before
 * anything else happens, so the rest of the body thinks in glass pixels
 * exactly as the 1:1 path does. */
static int app_present_rect(const uint8_t *src_le, uint32_t x, uint32_t y,
			    uint32_t w, uint32_t h, uint32_t pitch)
{
	const uint32_t div = CONFIG_ASTER_APP_SPACE_DIV;
	uint32_t dx0 = x * div, dx1 = (x + w) * div;
	uint32_t dy0 = y * div, dy1 = (y + h) * div;
	uint32_t bw, rows;
	int err;

	if (dx0 >= dx1 || dy0 >= dy1) {
		return ASTER_OK;
	}
	bw = dx1 - dx0;
	rows = swap_buf ? (uint32_t)(swap_buf_bytes / (bw * 2u)) : 1u;
	if (rows == 0) {
		rows = 1;
	}
	if (rows > dy1 - dy0) {
		rows = dy1 - dy0;
	}

	for (uint32_t dy = dy0; dy < dy1; dy += rows) {
		uint32_t n = dy1 - dy < rows ? dy1 - dy : rows;
		uint16_t *band = (uint16_t *)swap_buf;

		if (swap_buf) {
			for (uint32_t r = 0; r < n; r++) {
				const uint16_t *srow =
					(const uint16_t *)(src_le +
							   (size_t)((dy + r) / div) *
								   pitch * 2u);

				for (uint32_t i = 0; i < bw; i++) {
					band[(size_t)r * bw + i] =
						sys_cpu_to_be16(srow[(dx0 + i) / div]);
				}
			}
		} else {
			/* No bounce (PSRAM staging off, allocation failed): one
			 * row at a time through a stack row. Narrow and slow, but
			 * correct, and this is the rare path. */
			uint16_t row[ASTER_APP_ROW_PX];
			const uint16_t *srow =
				(const uint16_t *)(src_le +
						   (size_t)(dy / div) * pitch * 2u);

			n = 1;
			for (uint32_t i = 0; i < bw; i++) {
				row[i] = sys_cpu_to_be16(srow[(dx0 + i) / div]);
			}
			err = aster_plat_display_write_at_ex((const uint8_t *)row,
							   dx0, dy, bw, 1, bw, true);
			if (err != ASTER_OK) {
				return err;
			}
			continue;
		}
		aster_plat_display_flush_drain();
		/* The band went out; a failure here is the wire's, and it is
		 * reported rather than flattened to EIO. This check used to read
		 * `!= ASTER_EIO`, which is true on SUCCESS (ASTER_OK is 0, ASTER_EIO
		 * is -7) -- so a sub-rect present shipped its first band, told
		 * the app it had failed, and stopped. Nothing on the reference
		 * board noticed, because the app's space there is the panel's
		 * and this path is not reached. */
		err = aster_plat_display_write_at_ex((const uint8_t *)band, dx0, dy,
						   bw, n, bw, true);
		if (err != ASTER_OK) {
			return err;
		}
	}
	return ASTER_OK;
}

#endif /* CONFIG_ASTER_APP_SPACE_DIV > 1 */

bool aster_plat_display_app_space(uint32_t *w, uint32_t *h)
{
	if (!disp_dev) {
		return false;
	}
#if CONFIG_ASTER_APP_SPACE_DIV > 1
	if (s_app_scaled) {
		app_geom(w, h);
		return true;
	}
#endif
	logical_geom(w, h);
	return true;
}

bool aster_plat_display_app_scaled(void)
{
#if CONFIG_ASTER_APP_SPACE_DIV > 1
	return disp_dev && s_app_scaled;
#else
	return false;
#endif
}

/* The inverse of the present map: the app pixel under a finger. x,y arrive as
 * LOGICAL pixels -- what aster_plat_input_poll() reports and what the input
 * mapping in aster_runtime.c hands over -- and leave as app pixels.
 *
 * Integer division, which is exactly the inverse of the replicate above: the
 * glass pixel at dest sits inside app pixel dest/DIV, and that is the pixel
 * the user aimed at. There is no rounding term, because the map is not a
 * ratio -- every app pixel owns a contiguous DIV x DIV block of glass, and
 * nothing is centred on a fractional boundary.
 *
 * The 1:1 build answers true immediately: app space IS the logical screen, so
 * there is nothing to map and no sample to drop. */
bool aster_plat_display_app_touch(uint32_t *x, uint32_t *y)
{
#if CONFIG_ASTER_APP_SPACE_DIV > 1
	uint32_t aw, ah, vx, vy;

	if (!disp_dev || !s_app_scaled) {
		return true;
	}
	app_geom(&aw, &ah);
	vx = *x / CONFIG_ASTER_APP_SPACE_DIV;
	vy = *y / CONFIG_ASTER_APP_SPACE_DIV;
	*x = vx < aw ? vx : aw - 1u;
	*y = vy < ah ? vy : ah - 1u;
#else
	ARG_UNUSED(x);
	ARG_UNUSED(y);
#endif
	return true;
}

/* Whole-frame app present. A frame of the app's own size can go through the
 * same double-buffered flip the 1:1 present uses, so the wire runs under the
 * app's next frame instead of in front of it; anything else is a sub-rect and
 * takes the banded path. */
int aster_plat_display_app_present(const uint8_t *src_le, uint32_t w, uint32_t h,
				 uint32_t pitch)
{
#if CONFIG_ASTER_APP_SPACE_DIV > 1
	if (disp_dev && s_app_scaled) {
		uint32_t aw, ah;

		app_geom(&aw, &ah);
		if (w == aw && h == ah && app_present_full(src_le, w, h, pitch) == ASTER_OK) {
			return ASTER_OK;
		}
		return aster_plat_display_write_app_at(src_le, 0, 0, w, h, pitch);
	}
#endif
	return aster_plat_display_write_at_ex(src_le, 0, 0, w, h, pitch, false);
}

int aster_plat_display_write_app_at(const uint8_t *buf, uint32_t x, uint32_t y,
				  uint32_t w, uint32_t h, uint32_t pitch)
{
#if CONFIG_ASTER_APP_SPACE_DIV > 1
	if (disp_dev && s_app_scaled) {
		uint32_t aw, ah;

		app_geom(&aw, &ah);
		if (!w || !h || x > aw || w > aw - x || y > ah || h > ah - y) {
			return ASTER_EINVAL;
		}
		if (!pitch) {
			pitch = w;
		}
		return app_present_rect(buf, x, y, w, h, pitch);
	}
#endif
	/* DIV=1: app space IS the logical screen, so this is the ordinary 1:1
	 * path with nothing in front of it. The whole scaling mechanism is
	 * above, inside the #if, and does not exist in this build. */
	return aster_plat_display_write_at_ex(buf, x, y, w, h, pitch, false);
}

int aster_plat_display_write_at(const uint8_t *buf, uint32_t x, uint32_t y,
			      uint32_t w, uint32_t h, uint32_t pitch)
{
	return aster_plat_display_write_at_ex(buf, x, y, w, h, pitch, false);
}

int aster_plat_display_write_at_ex(const uint8_t *buf, uint32_t x, uint32_t y,
				 uint32_t w, uint32_t h, uint32_t pitch,
				 bool be_pixels_swapped)
{
    if (!aster_plat_display_count()) return ASTER_ENODEV;
	if (!disp_dev) {
		return ASTER_EINVAL;
	}
	/* x,y,w,h,pitch are in LOGICAL space; derive the panel-native rect */
	uint32_t lw, lh, pw, ph;
	logical_geom(&lw, &lh);
	panel_geom(&pw, &ph);
	/* Not `x + w > lw`: w is app-supplied and an x/w pair near the top of
	 * the u32 range wraps the sum back into bounds, after which px/py/
	 * prw/prh below become bogus row offsets. Same shape as the checks
	 * fixed in aster_native.c; this is the last gate before the wire. */
	if (!w || !h || x > lw || w > lw - x || y > lh || h > lh - y) {
		return ASTER_EINVAL;
	}
	if (!pitch) {
		pitch = w;
	}
	const uint32_t row_bytes = pitch * 2;
	uint32_t px, py, prw, prh; /* panel rect */
	switch (s_rot) {
	case 90:  px = y;  py = lw - (x + w); prw = h; prh = w; break;
	case 180: px = pw - (x + w); py = ph - (y + h); prw = w; prh = h; break;
	case 270: px = lh - (y + h); py = x; prw = h; prh = w; break;
	default:  px = x; py = y; prw = w; prh = h; break;
	}

	/* Every branch below that reaches the wire reports its rect to the
	 * glass record, which is what keeps a screen-grab true across partial
	 * writes. `whole_panel` means this rect replaces every pixel. */
	const bool whole_panel = (prw == pw && prh == ph);

	/* Direct-send fast path: source rows are already in panel byte order
	 * (LVGL RGB565_SWAPPED), rotation is native and the rect is packed
	 * (pitch == width, guaranteed by LV_DRAW_BUF_STRIDE_ALIGN=1). The
	 * bytes are exactly what the panel wants: hand the (PSRAM) buffer
	 * straight to display_write -- the stock SPI driver sees a
	 * non-DMA-capable source and k_malloc+memcpy()s each chunk through
	 * internal RAM (its own bounce flush included). The whole staging
	 * gather AND the staging buffer drop out of the loop. With
	 * ASTER_DISP_ASYNC the same job goes to the worker thread instead:
	 * the UI thread returns here and keeps rendering while the wire
	 * streams; LVGL band-reuse safety comes from flush_wait_cb (see
	 * ui_lvgl.c), not from this call. */
	if (be_pixels_swapped && s_rot == 0 && pitch == prw) {
		/* Rows are packed at prw and already in panel order: exactly
		 * what the record wants (never aliased -- buf belongs to the
		 * caller and LVGL recycles band buffers). */
		glass_track(buf, px, py, prw, prh, prw, whole_panel);
#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
		/* Slot 0 is the LVGL/shared poster slot. Waiting for *this*
		 * slot's previous send is exactly the old "drain the single
		 * queued job" behaviour, so LVGL's band-reuse contract is
		 * unchanged; the second slot exists solely so a full-screen
		 * flip (see aster_plat_display_flip_rgb565) can overlap the
		 * wire with the next frame's rasterization. */
		struct aster_disp_job job = {
			.buf = buf,
			.desc = {
				.buf_size = (size_t)row_bytes * prh,
				.width = prw,
				.height = prh,
				.pitch = prw,
			},
			.px = px,
			.py = py,
		};

		disp_slot_post(ASTER_DISP_SHARED_SLOT, &job);
		return ASTER_OK;
#else
		struct aster_disp_job job = {
			.buf = buf,
			.desc = {
				.buf_size = (size_t)row_bytes * prh,
				.width = prw,
				.height = prh,
				.pitch = prw,
			},
			.px = px,
			.py = py,
		};
		return disp_direct_send(&job) ? ASTER_EIO : ASTER_OK;
#endif
	}

#if IS_ENABLED(CONFIG_ASTER_DISP_FLIP_DBUF)
	/* Whole logical screen, native (LE) pixels: this is a frame present,
	 * not a band update. Take the double-buffered flip so the wire runs
	 * concurrently with the caller's next frame instead of under it. The
	 * flip does its own byte swap, so unlike the SWAPPED branch above it
	 * accepts LE sources -- which is exactly what the GFX engine hands us
	 * (it rasterizes into the app surface in ABI LE RGB565).
	 *
	 * This is the path that makes full-screen app frames asynchronous:
	 * aster_plat_display_write_rgb565() and the engine's dirty-rect flush
	 * both land here, so neither needs to know the flip exists. */
	if (!be_pixels_swapped && s_rot == 0 && pitch == prw && prw == pw &&
	    prh == ph) {
		if (aster_plat_display_flip_rgb565(buf, w, h, pitch) == ASTER_OK) {
			return ASTER_OK;
		}
	}
#endif

	if (prw > ASTER_DISP_MAX_ROW_PX) {
		return ASTER_EINVAL;
	}
	if (!swap_buf || !swap_buf_bytes) {
		return ASTER_ENOMEM;
	}
	/* Whole panel: nothing of the old record survives, so restart it on
	 * the shadow and let the bands below fill it in (a single-band walk
	 * would fill it in one go anyway). Partial: leave the record as it is
	 * and composite onto it -- that is the case the shadow exists for. */
	if (whole_panel) {
		glass_rebase_full();
	}
#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
	/* The staging gather is synchronous; a queued direct-send job would
	 * interleave its CASET/RASET windows with ours mid-loop. */
	aster_plat_display_flush_drain();
#endif
	const uint32_t dst_stride = prw * 2; /* packed panel rows in the bounce */
	/* rows per SPI window: whatever the byte budget affords for this rect */
	uint32_t bounce_rows = (uint32_t)(swap_buf_bytes / dst_stride);

	if (bounce_rows == 0) {
		bounce_rows = 1; /* unreachable at MAX_ROW_PX: 320 px = 640 B */
	}
#if IS_ENABLED(CONFIG_ASTER_DISP_TIMING)
	uint32_t t_sw0 = k_cycle_get_32(), acc_sw = 0, acc_wr = 0;
#endif
	/* LVGL wrote buf via the CPU (write-back D-cache); reading it back
	 * from the same core is coherent without any maintenance -- the only
	 * non-cached reader here is GDMA, and it sources swap_buf, not buf.
	 * The old unconditional flush_range() below was leftover from the
	 * GDMA-from-PSRAM-direct path (where the DMA master DID read buf)
	 * and just wrote the whole rect back to PSRAM for nothing. Keep it
	 * behind a switch so the leftover can be forced on if buf ever
	 * becomes non-cacheable. */
	if (IS_ENABLED(CONFIG_ASTER_DISP_SRC_FLUSH)) {
		sys_cache_data_flush_range((void *)buf, (size_t)row_bytes * h);
	}

	k_mutex_lock(&disp_lock, K_FOREVER);
	/* staging gather always ends in DMA-shippable BE bytes: swap 16-bit
	 * words only when the source is LE (native). A SWAPPED source already
	 * holds panel order, so it is copied verbatim -- double-swapping would
	 * put red back as blue. Only reachable when rot != 0 or a ragged
	 * pitch; the clean rot0 SWAPPED rect took the direct-send above. */
	const bool swap16 = !be_pixels_swapped;
	for (uint32_t base = 0; base < prh; base += bounce_rows) {
		uint32_t rows = prh - base;
		if (rows > bounce_rows) {
			rows = bounce_rows;
		}
		/* Gather panel rows through the inverse rotation map. With the
		 * forward rect solved above (px,py,prw,prh), each panel pixel
		 * (px+i, py+j) pulls a known LOGICAL pixel of the source rect:
		 *   rot0   (x+i,      y+j)
		 *   rot90  (x+w-1-j,  y+i)     panel row j = fixed source col
		 *   rot180 (x+w-1-i,  y+h-1-j)
		 *   rot270 (x+j,      y+h-1-i)
		 * Byte swap rides along on the same copy: zero extra passes.
		 * 90/270 walk columns of the logical rect, so the cache flush
		 * above covers the whole region and the gather stays L2-hot
		 * for LVGL bands (<= a few KB per row step). */
		for (uint32_t r = 0; r < rows; r++) {
			uint32_t j = base + r; /* panel row within the rect */
			uint16_t *dst = (uint16_t *)(swap_buf + (size_t)r * dst_stride);
			switch (s_rot) {
			case 90: {
				const uint32_t sc = (w - 1) - j; /* source col */
				for (uint32_t i = 0; i < prw; i++) {
					const uint16_t *src = (const uint16_t *)
						(buf + (size_t)i * row_bytes);
					dst[i] = swap16 ? sys_cpu_to_be16(src[sc]) : src[sc];
				}
				break;
			}
			case 180: {
				const uint16_t *src = (const uint16_t *)
					(buf + (size_t)(h - 1 - j) * row_bytes);
				for (uint32_t i = 0; i < prw; i++) {
					dst[i] = swap16 ? sys_cpu_to_be16(src[(w - 1) - i])
							: src[(w - 1) - i];
				}
				break;
			}
			case 270: {
				const uint32_t sc = j; /* source col */
				for (uint32_t i = 0; i < prw; i++) {
					const uint16_t *src = (const uint16_t *)
						(buf + (size_t)(h - 1 - i) * row_bytes);
					dst[i] = swap16 ? sys_cpu_to_be16(src[sc]) : src[sc];
				}
				break;
			}
			default: {
				const uint16_t *src = (const uint16_t *)
					(buf + (size_t)j * row_bytes);
				for (uint32_t i = 0; i < prw; i++) {
					dst[i] = swap16 ? sys_cpu_to_be16(src[i]) : src[i];
				}
				break;
			}
			}
		}
		/* DMA reads swap_buf (DRAM or PSRAM): the just-swapped bytes
		 * are still dirty in the write-back D-cache and MUST be pushed
		 * out, or the SPI ships the previous frame's bytes (the swap
		 * silently "does nothing" -- exactly the red-still-blue tell). */
		uint32_t band_bytes = dst_stride * rows;
		sys_cache_data_flush_range(swap_buf, band_bytes);
		/* These bytes are final for this band, in panel order: composite
		 * them so the record keeps up (no-op without the shadow). The
		 * bounce is reused by the next band, hence never aliased. */
		glass_track(swap_buf, px, py + base, prw, rows, prw, false);
#if IS_ENABLED(CONFIG_ASTER_DISP_TIMING)
		uint32_t t_wr0 = k_cycle_get_32();
		acc_sw += t_wr0 - t_sw0;
#endif
		struct display_buffer_descriptor desc = {
			.buf_size = band_bytes,
			.width = prw,
			.height = rows,
			.pitch = prw,
		};
		int err = display_write(disp_dev, px, py + base, &desc, swap_buf);
#if IS_ENABLED(CONFIG_ASTER_DISP_TIMING)
		acc_wr += k_cycle_get_32() - t_wr0;
		t_sw0 = k_cycle_get_32();
		static uint32_t s_tim_n, s_tim_sw, s_tim_wr, s_tim_bytes;
		s_tim_n++;
		s_tim_sw += acc_sw;
		s_tim_wr += acc_wr;
		s_tim_bytes += band_bytes;
		if (s_tim_n >= 64) {
			uint32_t sw_us = k_cyc_to_us_floor32(s_tim_sw);
			uint32_t wr_us = k_cyc_to_us_floor32(s_tim_wr);
			LOG_INF("disp tim: n=64 avg_rect=%u B staged gather=%u us write=%u us (MB/s w=%u) cyc/sw=%u",
				s_tim_bytes / 64, sw_us, wr_us,
				(unsigned)((uint64_t)s_tim_bytes * 1000000 / 1048576 / (wr_us ? wr_us : 1)),
				s_tim_sw / 64);			s_tim_n = s_tim_sw = s_tim_wr = s_tim_bytes = 0;
		}
#endif
		if (err) {
			k_mutex_unlock(&disp_lock);
			return ASTER_EIO;
		}
	}
	k_mutex_unlock(&disp_lock);
	return ASTER_OK;
}

int aster_plat_display_write_rgb565(const uint8_t *buf, uint32_t w, uint32_t h, uint32_t pitch)
{
	return aster_plat_display_write_at(buf, 0, 0, w, h, pitch);
}

int aster_plat_display_clear(uint16_t rgb565)
{
	aster_display_info_t info;
	int err = aster_plat_display_get_info(0, &info);
	if (err) {
		return err;
	}
	uint16_t *row = k_malloc(info.width * sizeof(uint16_t));
	if (!row) {
		return ASTER_ENOMEM;
	}
	for (uint32_t x = 0; x < info.width; x++) {
		row[x] = rgb565;
	}
	err = aster_plat_display_write_rgb565((uint8_t *)row, info.width, 1, info.width);
	k_free(row);
	if (err) {
		return err;
	}
	/* full clear: write row-by-row is slow; do a single blanking-style fill via
	 * repeating writes of a small band */
	uint32_t band = 16;
	uint8_t *fb = k_malloc(band * info.width * 2);
	if (!fb) {
		return ASTER_ENOMEM;
	}
	for (size_t i = 0; i < band * (size_t)info.width; i++) {
		((uint16_t *)fb)[i] = rgb565;
	}
	for (uint32_t y = 0; y < info.height; y += band) {
		uint32_t hh = (info.height - y < band) ? (info.height - y) : band;
		/* must flow through write_at: that is where the panel's
		 * byte-order swap (and cache flush) lives -- a raw
		 * display_write here bypassed it and re-introduced the
		 * red-shows-blue bug for fills */
		if (aster_plat_display_write_at(fb, 0, y, info.width, hh,
					      info.width) != ASTER_OK) {
			k_free(fb);
			return ASTER_EIO;
		}
	}
	k_free(fb);
	return ASTER_OK;
}

int aster_plat_backlight_set(bool on, uint8_t level)
{
	if (!bl_dev) {
		return ASTER_ENODEV;
	}
	ARG_UNUSED(level); /* LED GPIO driver: on/off only (no PWM wired) */
	return on ? led_on(bl_dev, bl_last) : led_off(bl_dev, bl_last);
}

/* The panel choice can compile the driver out while the board devicetree
 * still describes the node; DEVICE_DT_GET would then reference a device
 * struct that no longer exists. Gate the lookup on the built capability so a
 * headless profile links cleanly. */
static const struct device *display_dev_get(void)
{
#if IS_ENABLED(CONFIG_ASTER_HAS_DISPLAY)
	return DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_display));
#else
	return NULL;
#endif
}

int aster_platform_init(void)
{
	disp_dev = display_dev_get();
	if (!disp_dev || !device_is_ready(disp_dev)) {
		if (IS_ENABLED(CONFIG_ASTER_HAS_DISPLAY)) {
			LOG_ERR("display not ready");
		} else {
			LOG_INF("display: panel disabled in this build");
		}
		disp_dev = NULL;
		return aster_input_platform_init();
	}

	int err = display_blanking_off(disp_dev);
	if (err && err != -ENOSYS) {
		LOG_WRN("blanking_off %d", err);
	}

	/* Preprocessor guard, not `if (IS_ENABLED(...))`: a build without the LED
	 * subsystem must not reference the board's backlight node at all, or the
	 * asserts above would fire on a board that legitimately has no backlight
	 * and no `lcd-bl` alias. bl_dev stays NULL and backlight_set() returns
	 * ASTER_ENODEV, which is the correct answer for such a board. */
#if IS_ENABLED(CONFIG_LED)
	bl_dev = DEVICE_DT_GET_OR_NULL(DT_PARENT(DT_ALIAS(lcd_bl)));
	if (bl_dev && device_is_ready(bl_dev)) {
		(void)led_on(bl_dev, 0);
	} else {
		bl_dev = NULL;
	}
#endif


	uint32_t pw, ph, l_w, l_h;
	panel_geom(&pw, &ph);
	logical_geom(&l_w, &l_h);

#if IS_ENABLED(CONFIG_ASTER_DISP_ASYNC)
	disp_slots_init();
#endif

#if IS_ENABLED(CONFIG_ASTER_DISP_PSRAM_STAGE)
	/* One full-panel BE RGB565 staging buffer in PSRAM. 32-byte align
	 * matches GDMA / cache-line comfort on S3. */
	swap_buf_bytes = (size_t)pw * (size_t)ph * 2u;
	swap_buf = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, 32,
						   swap_buf_bytes);
	if (!swap_buf) {
		LOG_ERR("PSRAM display staging %zu B failed", swap_buf_bytes);
		swap_buf_bytes = 0;
		return -ENOMEM;
	}
	LOG_INF("display staging: PSRAM %p %zu B (one SPI window/rect)",
		(void *)swap_buf, swap_buf_bytes);
#else
	LOG_INF("display staging: DRAM bounce %u KB",
		(unsigned)CONFIG_ASTER_DISP_BOUNCE_KB);
#endif

#if IS_ENABLED(CONFIG_ASTER_SCREENSHOT)
	/* Screen-grab shadow: the panel-sized buffer partial writes composite
	 * onto (see the glass record above write_at_ex). Panel-sized and in
	 * PSRAM for the same reason the staging bounce is -- 150 KB has no
	 * business in DRAM, and the panel is the only geometry a grab can be
	 * in. Failure is not fatal: the record then only ever aliases whole
	 * frames, which is the v1 behaviour. */
	s_glass_buf_bytes = (size_t)pw * (size_t)ph * 2u;
	s_glass_buf = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, 32,
						      s_glass_buf_bytes);
	if (!s_glass_buf) {
		LOG_WRN("screen-grab shadow %zu B failed; a grab after a "
			"partial write falls back to the LVGL snapshot",
			s_glass_buf_bytes);
		s_glass_buf_bytes = 0;
	} else {
		/* Seed the record rather than waiting for the first whole-panel
		 * write: the band UI (LVGL) never issues one, and without a seed
		 * the record would stay empty and every grab of it would take
		 * the lv_snapshot_take() path. The GRAM is blanked to black just
		 * below, so "all zero" is the truth at this point. */
		memset(s_glass_buf, 0, s_glass_buf_bytes);
		s_glass = s_glass_buf;
		s_glass_w = pw;
		s_glass_h = ph;
		s_glass_pitch = pw;
		s_glass_pinned = false;
		LOG_INF("screen-grab shadow: PSRAM %p %zu B", (void *)s_glass_buf,
			s_glass_buf_bytes);
	}
#endif

#if IS_ENABLED(CONFIG_ASTER_DISP_FLIP_DBUF)
	/* Front buffers for the async flip path (see
	 * aster_plat_display_flip_rgb565). They live in PSRAM, exactly like the
	 * LVGL band buffers that the direct-send fast path already feeds the
	 * wire, so the driver's per-chunk bounce behaves identically; and they
	 * are sized to the PANEL because the flip path is rot0-only. This is
	 * independent of the staging buffer above -- the flip path never
	 * touches swap_buf. */
	{
		const uint32_t fw = (s_rot % 180) ? ph : pw;
		const uint32_t fh = (s_rot % 180) ? pw : ph;

		front_bytes = (size_t)fw * (size_t)fh * 2u;
		for (int i = 0; i < ASTER_DISP_FRONT_N; i++) {
			s_front[i] = shared_multi_heap_aligned_alloc(
				SMH_REG_ATTR_EXTERNAL, 32, front_bytes);
			if (!s_front[i]) {
				LOG_ERR("flip front buffer %d (%zu B) failed; "
					"full-screen presents stay synchronous",
					i, front_bytes);
				while (i-- > 0) {
					shared_multi_heap_free(s_front[i]);
					s_front[i] = NULL;
				}
				front_bytes = 0;
				break;
			}
			s_front_ok = true;
		}
		if (s_front_ok) {
			/* One-shot guard on the single pixel-format assumption
			 * this path rests on: the 32-bit SWAR in
			 * flip_rgb565() and the scalar sys_cpu_to_be16()
			 * used by write_at_ex()/the odd tail must yield the
			 * same panel bytes. They silently disagreed once --
			 * the SWAR was `(v << 16) | (v >> 16)`, a half-word
			 * rotate that reorders the two PIXELS and leaves each
			 * pixel's own bytes alone -- and no screen-grab could
			 * see it, because aster_gfx_last_frame() reads the
			 * app-side buffer, whose byte order stayed correct
			 * while the wire got swapped pixels. Probe with two
			 * real palette words (snake bg, pure red). */
			const uint16_t probe[2] = { 0x0863u, 0xF800u };
			const uint32_t v = (uint32_t)probe[0] | ((uint32_t)probe[1] << 16);
			const uint32_t sw = ((v & 0x00FF00FFu) << 8) |
					    ((v & 0xFF00FF00u) >> 8);
			const bool swar_ok =
				(uint16_t)sw == sys_cpu_to_be16(probe[0]) &&
				(uint16_t)(sw >> 16) == sys_cpu_to_be16(probe[1]);

			__ASSERT(swar_ok, "flip SWAR 0x%08x != sys_cpu_to_be16", sw);
			if (!swar_ok) {
				LOG_ERR("flip byte-swap 0x%08x disagrees with "
					"sys_cpu_to_be16; full-screen presents "
					"would ship wrong colours", sw);
			}
			LOG_INF("display flip: %d front buffers x %zu B in PSRAM",
				ASTER_DISP_FRONT_N, front_bytes);
		}
	}
#endif



#if CONFIG_ASTER_APP_SPACE_DIV > 1
	/* App space is the logical screen divided by ASTER_APP_SPACE_DIV; this
	 * only decides whether the two agree. A DIV=1 build has no app-space
	 * code to initialise and no state to set -- app space IS the logical
	 * screen there, by definition rather than by computation. */
	app_geom_init();
#endif

	LOG_INF("platform ready: panel %ux%u -> logical %ux%u (rot %d)",
		(unsigned)pw, (unsigned)ph, (unsigned)l_w, (unsigned)l_h, s_rot);
	/* The GRAM still holds power-on noise; blank it before anything
	 * takes ~5 s (storage + font streaming) to paint the first frame. */
	(void)aster_plat_display_clear(0x0000);
	(void)aster_plat_display_flush_drain();
	return aster_input_platform_init();
}
