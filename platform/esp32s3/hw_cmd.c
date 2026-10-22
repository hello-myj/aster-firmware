#include <zephyr/sys/util.h>
#include "audio_format.h"
/* SPDX-License-Identifier: MIT
 * `hw` shell: raw interface control — backlight, display, touch, IMU,
 * camera, audio, wifi. Basic device control without a system UI (V1 requirement).
 * SD file ops live under `aster fs` (the unique entry).
 */
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/display.h>
#include <zephyr/fs/fs.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <aster/audio.h>
#include <aster/display.h>
#include <aster/event.h>
#include "aster_platform.h"
#include "aster_runtime.h"
#include "aster_util.h"
#include "aster_storage.h"
#include "aster_mem.h"
#if IS_ENABLED(CONFIG_ASTER_IMGDEC)
#include "imgdec.h"
#endif

LOG_MODULE_REGISTER(hw_cmd, LOG_LEVEL_INF);

/* ---------- backlight ---------- */

static int cmd_bl(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: hw backlight <on|off>");
		return -EINVAL;
	}
	bool on = strcmp(argv[1], "on") == 0;
	int err = aster_plat_backlight_set(on, 255);
	if (err) {
		shell_print(sh, "backlight error %d", err);
	} else {
		shell_print(sh, "backlight %s", on ? "on" : "off");
	}
	return 0;
}

static int cmd_bl_pulse(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	for (int i = 0; i < 6; i++) {
		(void)aster_plat_backlight_set(false, 0);
		k_msleep(120);
		(void)aster_plat_backlight_set(true, 255);
		k_msleep(120);
	}
	shell_print(sh, "backlight pulse done");
	return 0;
}

/* ---------- display ---------- */

static int cmd_disp_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	aster_display_info_t d;
	int err = aster_plat_display_get_info(0, &d);
	if (err) {
		shell_error(sh, "no display");
		return 0;
	}
	/* refresh_rate and dpi_x100 are 0 = "unknown" by ABI convention, so an
	 * unset board prints 0 here rather than a plausible-looking constant --
	 * which is what these two fields used to carry. native_format is read
	 * from the struct instead of spelled "RGB565" in the format string, so
	 * it cannot go stale the way that literal would have.
	 *
	 * `accepts` is generated from ASTER_FORMAT_TABLE, one name per row, so
	 * this line can neither claim a format the port cannot convert nor omit
	 * one it can. That second failure is the one that actually happened:
	 * RGB888 was implemented and accepted while being advertised nowhere. */
#define ASTER_FORMAT_NAME(fmt_, bpp_, fn_) " " #fmt_
	shell_print(sh, "display0: %ux%u fmt=%#x refresh=%uHz dpi_x100=%u orient=%s",
		    d.width, d.height, d.native_format, d.refresh_rate, d.dpi_x100,
		    d.orientation == ASTER_ORIENT_LANDSCAPE ? "landscape" : "portrait");
	/* Printed unconditionally, including when it repeats the line above.
	 * The repetition IS the answer -- "this board hands apps the same
	 * space the glass has" and "this board scales" are different boards,
	 * and a reader who only sees one number cannot tell which one they
	 * are looking at. */
	shell_print(sh, "  app space: %ux%u%s", d.app_width, d.app_height,
		    (d.app_width == d.width && d.app_height == d.height)
			    ? " (1:1, no scaling compiled in)"
			    : " (scaled)");
	shell_print(sh, "  accepts:" ASTER_FORMAT_TABLE(ASTER_FORMAT_NAME));
#undef ASTER_FORMAT_NAME
	return 0;
}

static int cmd_disp_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	int err = aster_plat_display_clear(0x0000);
	shell_print(sh, err ? "clear failed" : "cleared black");
	return 0;
}

static int cmd_disp_fill(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: hw display fill <hex565|hex888>");
		return -EINVAL;
	}
	uint32_t v = (uint32_t)strtoul(argv[1], NULL, 16);
	uint16_t rgb;
	if (v > 0xFFFF) { /* treat as 888 */
		uint8_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
		rgb = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
	} else {
		rgb = (uint16_t)v;
	}
	int err = aster_plat_display_clear(rgb);
	shell_print(sh, err ? "fill failed" : "filled 0x%04x", rgb);
	return 0;
}

#if IS_ENABLED(CONFIG_ASTER_QSPI_TUNING)
static int cmd_disp_qspi(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argc);uint32_t mhz=strtoul(argv[1],NULL,10),flag=strtoul(argv[2],NULL,10),effective=0;
 if(flag>1) return -EINVAL;
 int err=aster_plat_display_qspi_tune(mhz,flag!=0,&effective);
 shell_print(sh,"QSPI TUNE result=%d requested=%u MHz effective=%u Hz prefetch=%u",err,mhz,effective,flag);
 return err;
}
#endif

#if IS_ENABLED(CONFIG_ASTER_AXS_WINDOW_TEST)
static int cmd_disp_windowtest(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argc);ARG_UNUSED(argv);
 aster_app_ctx_t *ctx=aster_app_current();
 if(!ctx||!ctx->paused) {shell_error(sh,"pause the foreground APP first");return -EBUSY;}
 uint32_t completed=0;int err=aster_plat_display_window_test(&completed);
 shell_print(sh,"AXS WINDOWTEST result=%d baseline=307200 B completed_patch_bytes=%u",err,completed);
 shell_print(sh,"Expected: RED 32x32 at (16,16), GREEN at (144,224), BLUE at (272,432)");
 shell_print(sh,"Physical screen/photo required; software shadow contains baseline only");
 return err;
}
#endif

static int cmd_disp_colorbar(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	static const uint16_t bar[8] = {
		0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000
	};
	aster_display_info_t d;
	if (aster_plat_display_get_info(0, &d) != ASTER_OK) {
		shell_error(sh, "no display");
		return 0;
	}
	uint32_t seg = d.width / 8;
	uint8_t *row = k_malloc(d.width * 2);
	if (!row) {
		return -ENOMEM;
	}
	for (uint32_t y = 0; y < d.height; y++) {
		for (uint32_t x = 0; x < d.width; x++) {
			int bidx = (int)(x / (seg ? seg : 1));
			if (bidx > 7) {
				bidx = 7;
			}
			((uint16_t *)row)[x] = bar[bidx];
		}
		/* via the platform API: a headless build (driver compiled
		 * out) must not hard-reference the DT device symbol here */
		if (aster_plat_display_write_at(row, 0, y, d.width, 1, d.width) != ASTER_OK) {
			shell_error(sh, "write row %u failed", y);
			break;
		}
	}
	k_free(row);
	shell_print(sh, "colorbar drawn");
	return 0;
}

static int cmd_disp_rot(const struct shell *sh, size_t argc, char **argv)
{
	/* The argument is matched as a TOKEN, not run through atoi(). It used to
	 * be `atoi(argv[1])`, and that turned the advertised `get` into a set:
	 * `atoi("get")` is 0, 0 is a legal rotation, so `hw display rot get`
	 * silently UNROTATED the panel and then printed "rotation=0" -- a
	 * modifier wearing a getter's name, which reads as a plain answer on a
	 * board that happened to already be at 0. Same defect as the usage
	 * string in the registration below (a grammar that was advertised and
	 * never implemented); this one fails louder, because a wrong number
	 * looks like a right one. */
	static const char *const degs[] = { "0", "90", "180", "270" };

	if (argc >= 2 && strcmp(argv[1], "get")) {
		int deg = -1;
		int err;

		for (size_t i = 0; i < ARRAY_SIZE(degs); i++) {
			if (!strcmp(argv[1], degs[i])) {
				deg = atoi(degs[i]);
				break;
			}
		}
		if (deg < 0) {
			shell_error(sh, "rot takes get|0|90|180|270");
			return 0;
		}
		err = aster_plat_display_rotation_set(deg);
		if (err) {
			shell_error(sh, "rot %s refused (%d)", argv[1], err);
			return 0;
		}
		/* re-probe geometry so the glass + info agree immediately */
	}
	shell_print(sh, "rotation=%d (set with: hw display rot <deg>)",
		    aster_plat_display_rotation_get());
	return 0;
}

/* Orientation changes share the same worker-owned path as aster display. */
static int cmd_disp_orient(const struct shell *sh, size_t argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "get")) {
		int want;
		int err;

		if (!strcmp(argv[1], "portrait")) {
			want = ASTER_ORIENT_PORTRAIT;
		} else if (!strcmp(argv[1], "landscape")) {
			want = ASTER_ORIENT_LANDSCAPE;
		} else {
			shell_error(sh, "orient takes get|portrait|landscape");
			return 0;
		}
		#if IS_ENABLED(CONFIG_ASTER_WAMR)
        err = aster_runtime_display_mode_request(want == ASTER_ORIENT_PORTRAIT
            ? ASTER_DISPLAY_MODE_PORTRAIT : ASTER_DISPLAY_MODE_LANDSCAPE);
#else
        err = aster_plat_display_orientation_set(want);
#endif
		if (err) {
			/* Two different refusals land here and the difference
			 * matters to whoever is holding the board: ENOTSUP is
			 * "this port cannot" -- no second window, and software
			 * rotation declined (it is unavailable when app-space
			 * scaling is on) -- while anything else is a
			 * driver-level failure. Printing one sentence for
			 * both would send someone to the wrong subsystem. */
			shell_error(sh, "cannot switch to %s (%d): %s",
				    argv[1], err,
				    err == ASTER_ENOTSUP
					    ? "no second window on this panel and "
					      "software rotation is unavailable here"
					    : "the panel driver refused it");
			return 0;
		}
	}
	shell_print(sh, "orientation=%s (set with: hw display orient "
		    "<portrait|landscape>)",
		    aster_plat_display_orientation_get() == ASTER_ORIENT_LANDSCAPE
			    ? "landscape" : "portrait");
	shell_print(sh, "  queued changes rebuild the desktop on the UI worker");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_disp,
#if IS_ENABLED(CONFIG_ASTER_QSPI_TUNING)
 SHELL_CMD_ARG(qspi,NULL,"<40|48|60|80 MHz> <prefetch 0|1>",cmd_disp_qspi,3,0),
#endif
	SHELL_CMD(info, NULL, "display info", cmd_disp_info),
	SHELL_CMD(clear, NULL, "fill black", cmd_disp_clear),
	SHELL_CMD_ARG(fill, NULL, "fill <hex>", cmd_disp_fill, 2, 0),
	SHELL_CMD(colorbar, NULL, "8-bar color test", cmd_disp_colorbar),
#if IS_ENABLED(CONFIG_ASTER_AXS_WINDOW_TEST)
    SHELL_CMD(windowtest,NULL,"raw CASET/RASET/RAMWR patches (paused APP)",cmd_disp_windowtest),
#endif
	/* These two read "get|set <...>" -- copied from a design where the verb
	 * took an opcode the way `hw touch tap` does. It does not: the argument
	 * is bare, and the advertised spelling died in the shell's own count
	 * check ("wrong parameter count") before the handler ever saw it, so
	 * the one thing the help was for -- telling someone how to call it --
	 * was the one thing it got wrong. The canonical reminder is printed by
	 * the handler itself ("set with: ..."), so these stay short. */
	SHELL_CMD_ARG(rot, NULL, "get|<0|90|180|270>: software rotation",
		      cmd_disp_rot, 1, 1),
	SHELL_CMD_ARG(orient, NULL, "get|<portrait|landscape>: turn the panel "
		      "now, no reboot", cmd_disp_orient, 1, 1),
	SHELL_SUBCMD_SET_END
);

/* ---------- touch ---------- */

/* Upper bound on the `hw touch` sampling loop, in 10 ms iterations. The loop
 * runs inside the shell thread and cannot be interrupted (see the note at the
 * clamp below), so this is what keeps a typo'd count from taking the console
 * away for minutes. 3000 = 30 s, generous for holding a finger on the glass. */
#define HW_TOUCH_MAX_ITER 3000

static int cmd_touch(const struct shell *sh, size_t argc, char **argv)
{
	/* hw touch                              -> sample/drain (diagnostic)
	 * hw touch tap   <x> <y>                -> down+up on one point
	 * hw touch down  <x> <y> / move / up    -> a drag, in pieces
	 * hw touch drag  <x1> <y1> <x2> <y2>    -> down, hold, up (a swipe)
	 * hw touch map   <x> <y>                -> where an app would see it
	 * Coordinates are LOGICAL: what an app lays out in, i.e. what a
	 * screenshot shows. `drag` is how a scroll or a skip reaches an app
	 * whose gesture code reads the DOWN and UP points. */
	if (argc >= 2) {
		const char *op = argv[1];
		uint32_t x = 0, y = 0;
#if defined(CONFIG_ASTER_AXS15231B_RAW_DIAG)
		if (strcmp(op,"replay") == 0) {
			if (argc != 2) return -EINVAL;
			int err=aster_plat_axs_touch_diag_replay_start();
			shell_print(sh,"injected touch driver replay result=%d",err);
			return err;
		}
#endif

		if (strcmp(op, "state") == 0) {
			uint32_t pts[ASTER_PLAT_MAX_TOUCH * 4] = {0};
			aster_plat_input_state(pts, ASTER_PLAT_MAX_TOUCH);
			uint32_t count = 0;
			for (; count < ASTER_PLAT_MAX_TOUCH && pts[count*4+2] != ASTER_INPUT_TERM; count++) {
				shell_print(sh, "touch slot=%u x=%u y=%u action=%u",
					pts[count*4+3], pts[count*4], pts[count*4+1], pts[count*4+2]);
			}
			shell_print(sh, "touch active=%u", count);
			aster_touch_debug_t health;
			aster_plat_input_touch_debug(&health);
			shell_print(sh,"touch recovery=%u errors=%u pending=%u hardware=%#x synthetic=%#x",
				health.recovery,health.errors,health.pending,
				health.hardware_mask,health.synthetic_mask);
			return 0;
		}
		if (strcmp(op, "slot") == 0) {
			if (argc != 6) {
				shell_error(sh, "usage: hw touch slot <0|1> <down|move|up> <x> <y>");
				return -EINVAL;
			}
			uint32_t values[3];
			const unsigned positions[3] = {2,4,5};
			for (unsigned i=0; i<3; i++) {
				char *end;
				unsigned long value=strtoul(argv[positions[i]],&end,0);
				if (!argv[positions[i]][0] || *end || argv[positions[i]][0]=='-' || value>UINT16_MAX)
					return -EINVAL;
				values[i]=(uint32_t)value;
			}
			uint32_t action = !strcmp(argv[3],"down") ? ASTER_TOUCH_DOWN :
				!strcmp(argv[3],"move") ? ASTER_TOUCH_MOVE :
				!strcmp(argv[3],"up") ? ASTER_TOUCH_UP : 0;
			if (values[0]>=ASTER_PLAT_MAX_TOUCH || !action) return -EINVAL;
			aster_plat_input_touch_inject_slot(values[0],values[1],values[2],action);
			shell_print(sh,"injected touch slot=%u %s x=%u y=%u",
				values[0],argv[3],values[1],values[2]);
			return 0;
		}

		/* The inverse map, asked rather than inferred. On a board that
		 * scales, every value an app receives has been through it, so
		 * "the app saw the wrong pixel" and "the app was handed the
		 * wrong pixel" are different bugs with the same symptom -- and
		 * without this the only way to tell them apart is to read the
		 * app's source. Prints both ends so the line says what the
		 * divisor actually did. */
		if (strcmp(op, "map") == 0) {
			if (argc < 4) {
				shell_error(sh, "usage: hw touch map <x> <y>");
				return -EINVAL;
			}
			uint32_t lx = (uint32_t)strtoul(argv[2], NULL, 0);
			uint32_t ly = (uint32_t)strtoul(argv[3], NULL, 0);
			uint32_t ax = lx, ay = ly;

			(void)aster_plat_display_app_touch(&ax, &ay);
			shell_print(sh, "touch map: logical %ux%u -> app %ux%u",
				    lx, ly, ax, ay);
			return 0;
		}
		if (strcmp(op, "up") == 0) {
			/* release where the finger is, not at the origin:
			 * UP carries a position and apps use it */
			uint32_t pts[ASTER_PLAT_MAX_TOUCH * 4 + 4] = {0};

			aster_plat_input_state(pts, ASTER_PLAT_MAX_TOUCH);
			if (pts[2] == ASTER_INPUT_TERM) {
				shell_print(sh, "injected touch up: already released");
				return 0;
			}
			aster_plat_input_touch_inject(pts[0], pts[1], ASTER_TOUCH_UP);
			shell_print(sh, "injected touch up x=%u y=%u", pts[0], pts[1]);
			return 0;
		}
		if (strcmp(op, "tap") == 0 || strcmp(op, "down") == 0 ||
		    strcmp(op, "move") == 0) {
			if (argc < 4) {
				shell_error(sh, "usage: hw touch %s <x> <y>", op);
				return -EINVAL;
			}
			x = (uint32_t)strtoul(argv[2], NULL, 0);
			y = (uint32_t)strtoul(argv[3], NULL, 0);
			if (strcmp(op, "down") == 0) {
				aster_plat_input_touch_inject(x, y, ASTER_TOUCH_DOWN);
			} else if (strcmp(op, "move") == 0) {
				aster_plat_input_touch_inject(x, y, ASTER_TOUCH_MOVE);
			} else {
				aster_plat_input_touch_inject(x, y, ASTER_TOUCH_DOWN);
				k_msleep(40); /* as long as a hand is, roughly */
				aster_plat_input_touch_inject(x, y, ASTER_TOUCH_UP);
			}
			shell_print(sh, "injected touch %s x=%u y=%u", op, x, y);
			return 0;
		}
		if (strcmp(op, "drag") == 0) {
			uint32_t x2, y2;
			/* Optional: how many move samples to spread the stroke
			 * over, and how long to hold between them. The default
			 * (one move, 40 ms) is a flick; `steps 8` is a slow
			 * deliberate drag, which is what a recognizer that
			 * wants several samples in a row has to be tested
			 * with -- the default would never give it one. */
			uint32_t steps = 1, step_ms = 40;

			if (argc < 6) {
				shell_error(sh, "usage: hw touch drag <x1> <y1>"
					    " <x2> <y2> [steps] [step_ms]");
				return -EINVAL;
			}
			x = (uint32_t)strtoul(argv[2], NULL, 0);
			y = (uint32_t)strtoul(argv[3], NULL, 0);
			x2 = (uint32_t)strtoul(argv[4], NULL, 0);
			y2 = (uint32_t)strtoul(argv[5], NULL, 0);
			if (argc > 6) {
				steps = (uint32_t)strtoul(argv[6], NULL, 0);
			}
			if (argc > 7) {
				step_ms = (uint32_t)strtoul(argv[7], NULL, 0);
			}
			if (steps < 1 || steps > 64) {
				steps = 1; /* keep the shell call bounded */
			}
			aster_plat_input_touch_inject(x, y, ASTER_TOUCH_DOWN);
			k_msleep(40);
			for (uint32_t i = 1; i <= steps; i++) {
				aster_plat_input_touch_inject(
					(uint32_t)((int64_t)x + ((int64_t)x2 - x) * i / steps),
					(uint32_t)((int64_t)y + ((int64_t)y2 - y) * i / steps),
					ASTER_TOUCH_MOVE);
				k_msleep(step_ms);
			}
			aster_plat_input_touch_inject(x2, y2, ASTER_TOUCH_UP);
			shell_print(sh, "injected touch drag %u,%u -> %u,%u"
				    " (%u moves, %u ms apart)", x, y, x2, y2,
				    steps, step_ms);
			return 0;
		}
		if (strcmp(op, "help") == 0) {
			shell_print(sh, "hw touch state | slot <0|1> <down|move|up> <x> <y>");
#if defined(CONFIG_ASTER_AXS15231B_RAW_DIAG)
			shell_print(sh,"hw touch replay: bounded lost-UP driver fault fixture");
#endif
			shell_print(sh, "hw touch [n] | tap <x> <y> | down <x> <y> |"
					" move <x> <y> | up | drag <x1> <y1> <x2> <y2>"
					" [steps] [step_ms]");
			return 0;
		}
	}
	int n = 1000; /* iterations */
	if (argc >= 2) {
		n = atoi(argv[1]);
	}
	if (!aster_plat_input_touch_available()) {
		shell_warn(sh, "no touch device");
		return 0;
	}
	/* This used to say "(ctrl-c to stop)", which was false. Zephyr's ^C
	 * (shell.c, SHELL_VT100_ASCII_CTRL_C) only cancels the readline; while
	 * a handler is running the shell thread is *inside* it and never sees
	 * the byte. Measured 2026-09-22: `hw touch 60000` locked the console
	 * for the full 10 minutes, ^C did nothing, and the only way back was a
	 * DTR/RTS reset. Clamp instead, and say so out loud -- the fix for a
	 * silent death is to make it audible, not to promise an abort we
	 * cannot deliver. */
	if (n > HW_TOUCH_MAX_ITER) {
		shell_warn(sh, "clamped to %d samples (%d s);"
			   " this command cannot be interrupted",
			   HW_TOUCH_MAX_ITER, HW_TOUCH_MAX_ITER / 100);
		n = HW_TOUCH_MAX_ITER;
	}
	if (n < 1) {
		n = 1;
	}
	shell_print(sh, "touch: %d samples (%d s)", n, n / 100);
	uint32_t pts[ASTER_PLAT_MAX_TOUCH * 4 + 4];
	uint64_t last_report = 0;
	for (int i = 0; i < n; i++) {
		aster_plat_input_state(pts, ASTER_PLAT_MAX_TOUCH);
		if (pts[2] != 0xFFFFFFFF && (uint64_t)k_uptime_get() - last_report > 100) {
			last_report = (uint64_t)k_uptime_get();
			shell_print(sh, "touch x=%u y=%u slot=%u", pts[0], pts[1], pts[3]);
		}
		k_msleep(10);
	}
	return 0;
}

/* ---------- key ---------- */

static int cmd_key(const struct shell *sh, size_t argc, char **argv)
{
	/* hw key                 -> poll held + drain edges (diagnostic)
	 * hw key inject <c> down|up -> synthesize a key edge (for tests) */
	if (argc >= 2 && strcmp(argv[1], "inject") == 0) {
		if (argc < 4) {
			shell_error(sh, "usage: hw key inject <code> down|up");
			return -EINVAL;
		}
		uint32_t code = (uint32_t)strtoul(argv[2], NULL, 0);
		bool down = strcmp(argv[3], "down") == 0;
		aster_plat_input_keys_inject(code, down);
		shell_print(sh, "injected key code=%u %s", code, down ? "down" : "up");
		return 0;
	}
	uint32_t held[ASTER_PLAT_MAX_KEYS];
	int n = aster_plat_input_keys_state(held, ASTER_PLAT_MAX_KEYS);
	shell_print(sh, "keys available=%d held=%d",
		    aster_plat_input_keys_available(), n);
	for (int i = 0; i < n; i++) {
		shell_print(sh, "  held code=%u", held[i]);
	}
	aster_key_evt_t ev[ASTER_PLAT_MAX_KEYS];
	int m = aster_plat_input_keys_poll(ev, ASTER_PLAT_MAX_KEYS);
	for (int i = 0; i < m; i++) {
		shell_print(sh, "  edge code=%u %s", ev[i].code,
			    ev[i].pressed ? "down" : "up");
	}
	return 0;
}

/* ---------- imu ---------- */

static int cmd_imu(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	float ax, ay, az, gx, gy, gz;
	int err = aster_plat_imu_read(&ax, &ay, &az, &gx, &gy, &gz);
	if (err) {
		shell_error(sh, "imu read failed (%d)", err);
		return 0;
	}
	shell_print(sh, "accel  %d %d %d mg", (int)(ax * (1000.0f / 9.80665f)), (int)(ay * (1000.0f / 9.80665f)),
		    (int)(az * (1000.0f / 9.80665f)));
	shell_print(sh, "gyro   %d %d %d mdps", (int)(gx * 57295.7795f), (int)(gy * 57295.7795f),
		    (int)(gz * 57295.7795f));
	return 0;
}

/* ---------- camera ---------- */

#if IS_ENABLED(CONFIG_ASTER_CAMERA)
static int cmd_cam_capture(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: hw cam capture <path.rgb>");
		return -EINVAL;
	}
	if (!aster_storage_mounted()) {
		shell_error(sh, "SD not mounted");
		return -EINVAL;
	}
	shell_print(sh, "capturing 320x240 RGB565 -> %s ...", argv[1]);
	int err = aster_plat_cam_capture_rgb565(argv[1]);
	shell_print(sh, err ? "capture failed (%d)" : "captured", err);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cam,
	SHELL_CMD_ARG(capture, NULL, "single frame to file", cmd_cam_capture, 2, 0),
	SHELL_SUBCMD_SET_END
);
#endif

/* ---------- audio ---------- */

#if IS_ENABLED(CONFIG_ASTER_AUDIO)

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void wav_header(uint8_t *h, uint32_t data_bytes)
{
	memcpy(h, "RIFF", 4);
	put_le32(h + 4, 36u + data_bytes);
	memcpy(h + 8, "WAVEfmt ", 8);
	put_le32(h + 16, 16);
	put_le16(h + 20, 1);
	put_le16(h + 22, (uint16_t)CONFIG_ASTER_AUDIO_CHANNELS);
	put_le32(h + 24, CONFIG_ASTER_AUDIO_RATE);
	put_le32(h + 28, CONFIG_ASTER_AUDIO_RATE * ASTER_PLAT_AUDIO_FRAME);
	put_le16(h + 32, (uint16_t)ASTER_PLAT_AUDIO_FRAME);
	put_le16(h + 34, (uint16_t)CONFIG_ASTER_AUDIO_BITS);
	memcpy(h + 36, "data", 4);
	put_le32(h + 40, data_bytes);
}

static int32_t pcm_peak(const uint8_t *pcm, size_t nbytes)
{
	const int16_t *s = (const int16_t *)pcm;
	size_t n = nbytes / sizeof(int16_t);
	int32_t peak = 0;

	for (size_t i = 0; i < n; i++) {
		int32_t v = s[i];
		if (v < 0) {
			v = -v;
		}
		if (v > 32767) {
			v = 32767;
		}
		if (v > peak) {
			peak = v;
		}
	}
	return peak;
}

static uint32_t audio_ms_bytes(uint32_t ms)
{
	if (ms < 20) {
		ms = 20;
	}
	if (ms > 3000) {
		ms = 3000;
	}
	return (CONFIG_ASTER_AUDIO_RATE * ASTER_PLAT_AUDIO_FRAME / 1000u) * ms;
}

static const char *audio_state_name(uint32_t st)
{
	switch (st) {
	case ASTER_AUDIO_PLAYING:
		return "playing";
	case ASTER_AUDIO_PAUSED:
		return "paused";
	default:
		return "idle";
	}
}

static int cmd_audio_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	uint8_t dac = 0, adc = 0;
	int err = aster_plat_audio_probe(&dac, &adc);
	shell_print(sh, "audio: %u Hz %u-bit stereo frame=%u  vol=%d",
		    CONFIG_ASTER_AUDIO_RATE, CONFIG_ASTER_AUDIO_BITS, ASTER_PLAT_AUDIO_FRAME,
		    aster_plat_audio_get_volume());
	shell_print(sh, "dac id=0x%02x  adc id=0x%02x  probe=%d", dac, adc, err);
	return 0;
}

static int cmd_audio_vol(const struct shell *sh, size_t argc, char **argv)
{
	if (argc >= 2) {
		int vol = (int)strtoul(argv[1], NULL, 0);
		int err = aster_plat_audio_set_volume(vol);
		if (err) {
			shell_error(sh, "vol set failed (%d) — use 0-100", err);
			return 0;
		}
	}
	shell_print(sh, "vol=%d", aster_plat_audio_get_volume());
	return 0;
}

static int cmd_audio_state(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	uint32_t st = 0, total = 0, done = 0, vol = 0;
	uint32_t ms = 0, tot_ms = 0;
	int err = aster_plat_audio_get_state(&st, &total, &done, &vol);

	if (err) {
		shell_print(sh, "state failed (%d)", err);
		return 0;
	}
	(void)aster_plat_audio_get_clock(&ms, &tot_ms);
	shell_print(sh, "state=%s bytes=%u/%u msec=%u/%u vol=%u",
		    audio_state_name(st), (unsigned)done, (unsigned)total,
		    (unsigned)ms, (unsigned)tot_ms, (unsigned)vol);
	return 0;
}

static int cmd_audio_seek(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms;
	int err;

	if (argc < 2) {
		shell_error(sh, "usage: hw audio seek <msec>");
		return 0;
	}
	ms = (uint32_t)strtoul(argv[1], NULL, 0);
	err = aster_plat_audio_seek(ms);
	if (err) {
		shell_print(sh, "seek failed (%d)", err);
	} else {
		shell_print(sh, "seek %u ms", (unsigned)ms);
	}
	return 0;
}

static int cmd_audio_pause(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int err = aster_plat_audio_pause();
	shell_print(sh, err ? "pause failed (%d)" : "paused", err);
	return 0;
}

static int cmd_audio_resume(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int err = aster_plat_audio_resume();
	shell_print(sh, err ? "resume failed (%d)" : "resumed", err);
	return 0;
}

static int cmd_audio_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int err = aster_plat_audio_stop();
	shell_print(sh, err ? "stop failed (%d)" : "stopped", err);
	return 0;
}

static int cmd_audio_beep(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = 400;
	uint32_t hz = 440;
	if (argc >= 2) {
		ms = (uint32_t)strtoul(argv[1], NULL, 0);
	}
	if (argc >= 3) {
		hz = (uint32_t)strtoul(argv[2], NULL, 0);
	}
	if (ms < 20) {
		ms = 20;
	}
	if (ms > 8000) {
		ms = 8000;
	}
	shell_print(sh, "beep %u ms @ %u Hz ...", ms, hz);
	int err = aster_plat_audio_beep(ms, hz);
	shell_print(sh, err ? "beep failed (%d)" : "beep done", err);
	return 0;
}

static int cmd_audio_rec(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: hw audio rec <path.wav> [ms]");
		return -EINVAL;
	}
	if (!aster_storage_mounted()) {
		shell_error(sh, "SD not mounted");
		return -EINVAL;
	}
	uint32_t ms = 1000;
	if (argc >= 3) {
		ms = (uint32_t)strtoul(argv[2], NULL, 0);
	}
	uint32_t nbytes = audio_ms_bytes(ms);
	uint8_t *pcm = aster_psram_alloc(nbytes);
	if (!pcm) {
		shell_error(sh, "oom %u bytes", nbytes);
		return -ENOMEM;
	}
	shell_print(sh, "recording %u ms (%u bytes) -> %s ...",
		    nbytes * 1000u / (CONFIG_ASTER_AUDIO_RATE * ASTER_PLAT_AUDIO_FRAME),
		    nbytes, argv[1]);
	int err = aster_plat_audio_record(pcm, nbytes);
	if (err) {
		aster_psram_free(pcm);
		shell_print(sh, "record failed (%d)", err);
		return 0;
	}
	struct fs_file_t f;
	fs_file_t_init(&f);
	err = fs_open(&f, argv[1], FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
	if (err) {
		aster_psram_free(pcm);
		shell_print(sh, "open failed (%d)", err);
		return 0;
	}
	uint8_t hdr[44];
	wav_header(hdr, nbytes);
	(void)aster_fs_write_safe(&f, hdr, sizeof(hdr));
	(void)aster_fs_write_safe(&f, pcm, nbytes);
	fs_close(&f);
	int32_t peak = pcm_peak(pcm, nbytes);
	aster_psram_free(pcm);
	shell_print(sh, "recorded peak=%d/32767%s", (int)peak,
		    peak == 0 ? " (silence — speak near the mics and retry)" : "");
	return 0;
}

static int cmd_audio_play(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: hw audio play <path.wav|.mp3>");
		return -EINVAL;
	}
	if (!aster_storage_mounted()) {
		shell_error(sh, "SD not mounted");
		return -EINVAL;
	}
	shell_print(sh, "playing %s ...", argv[1]);
	int err = aster_plat_audio_play_path(argv[1]);
	shell_print(sh, err ? "play failed (%d)" : "play done", err);
	return 0;
}

static int cmd_audio_start(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: hw audio start <path.wav|.mp3>");
		return -EINVAL;
	}
	if (!aster_storage_mounted()) {
		shell_error(sh, "SD not mounted");
		return -EINVAL;
	}
	int err = aster_plat_audio_start_path(argv[1]);
	if (err) {
		shell_print(sh, "start failed (%d)", err);
		return 0;
	}
	shell_print(sh, "started %s (background, streaming)", argv[1]);
	return 0;
}

static int cmd_audio_loop(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = 800;
	if (argc >= 2) {
		ms = (uint32_t)strtoul(argv[1], NULL, 0);
	}
	uint32_t nbytes = audio_ms_bytes(ms);
	uint8_t *pcm = aster_psram_alloc(nbytes);
	if (!pcm) {
		return -ENOMEM;
	}
	shell_print(sh, "loop: rec %u bytes then play ...", nbytes);
	int err = aster_plat_audio_record(pcm, nbytes);
	if (err) {
		aster_psram_free(pcm);
		shell_print(sh, "record failed (%d)", err);
		return 0;
	}
	int32_t peak = pcm_peak(pcm, nbytes);
	shell_print(sh, "recorded peak=%d/32767%s", (int)peak,
		    peak == 0 ? " (silence — playback will be quiet)" : "");
	err = aster_plat_audio_play(pcm, nbytes);
	aster_psram_free(pcm);
	shell_print(sh, err ? "play failed (%d)" : "loop done", err);
	return 0;
}

/* Explicit effect commands: no APP-frame timer is used to repeat cues. */
static bool fx_number(const char *text,uint32_t *value)
{
 char *end;errno=0;
 unsigned long long n=strtoull(text,&end,0);
 if(!*text || *text=='-' || *end || errno || n>UINT32_MAX) return false;
 *value=(uint32_t)n;return true;
}
static int cmd_audio_fxload(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argc);
 int h=aster_plat_audio_effect_load(argv[1]);shell_print(sh,"fxload handle=%d",h);
 return h<0?h:0;
}
static int cmd_audio_fx(const struct shell *sh,size_t argc,char **argv)
{
 uint32_t a[6]={0};
 for(size_t i=1;i<argc;i++) if(i>6 || !fx_number(argv[i],&a[i-1])) return -EINVAL;
 int rc;
 if(!strcmp(argv[0],"fxfree")) rc=aster_plat_audio_effect_unload(a[0]);
 else if(!strcmp(argv[0],"fxstop")) rc=aster_plat_audio_effect_stop(a[0],a[1]);
 else if(!strcmp(argv[0],"fxparams")) {
  aster_audio_effect_params_t p={sizeof(p),1,a[1],a[2],a[3]};
  rc=aster_plat_audio_effect_set_params(a[0],&p);
 } else {
  aster_audio_effect_play_desc_t d={.size=sizeof(d),.version=1,
   .flags=!strcmp(argv[0],"fxloop")?ASTER_AUDIO_EFFECT_F_LOOP:0,
   .loop_start_frame=a[1],.loop_end_frame=a[2],.crossfade_frames=a[3],
   .pitch_q16=65536,.gain_q16=65536};
  rc=aster_plat_audio_effect_play_ex(a[0],&d);
 }
 shell_print(sh,"%s result=%d",argv[0],rc);return rc;
}
static int cmd_audio_fxstate(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argc);ARG_UNUSED(argv);
 aster_audio_effect_debug_t d;aster_plat_audio_effect_debug(&d);
 shell_print(sh,"effects output_hz=%u loaded=0x%x playing=0x%x suspended=%u pcm_bytes=%u mixed_peak=%u",
  d.rate,d.loaded_mask,d.playing_mask,d.suspended,d.pcm_bytes,d.mixed_peak);
 for(unsigned i=0;i<4;i++) shell_print(sh,"slot=%u rendered_source_frame=%u fraction_q16=%u",i+1,
  (uint32_t)(d.phase_q16[i]>>16),(uint32_t)(d.phase_q16[i]&65535));
 shell_print(sh,"DMA live depth=unknown; hardware underruns=unknown");return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_audio,
	SHELL_CMD_ARG(fxload, NULL, "preload PCM16 WAV <path>", cmd_audio_fxload, 2, 0),
	SHELL_CMD_ARG(fxplay, NULL, "one-shot <handle>", cmd_audio_fx, 2, 0),
	SHELL_CMD_ARG(fxloop, NULL, "loop <handle> [start end crossfade] (source frames)", cmd_audio_fx, 2, 3),
	SHELL_CMD_ARG(fxparams, NULL, "<handle> <pitch_q16> <gain_q16> <ramp_ms>", cmd_audio_fx, 5, 0),
	SHELL_CMD_ARG(fxstop, NULL, "<handle> [fade_ms]", cmd_audio_fx, 2, 1),
	SHELL_CMD_ARG(fxfree, NULL, "unload <handle>", cmd_audio_fx, 2, 0),
	SHELL_CMD(fxstate, NULL, "effect phase/cache snapshot (not DMA telemetry)", cmd_audio_fxstate),
	SHELL_CMD(info, NULL, "codec / format info", cmd_audio_info),
	SHELL_CMD_ARG(vol, NULL, "get/set volume [0-100]", cmd_audio_vol, 1, 1),
	SHELL_CMD_ARG(beep, NULL, "sine [ms] [hz]", cmd_audio_beep, 1, 2),
	SHELL_CMD_ARG(rec, NULL, "record wav <path> [ms]", cmd_audio_rec, 2, 1),
	SHELL_CMD_ARG(play, NULL, "blocking play wav/mp3 <path>", cmd_audio_play, 2, 0),
	SHELL_CMD_ARG(start, NULL, "background play wav/mp3 <path>", cmd_audio_start, 2, 0),
	SHELL_CMD(pause, NULL, "pause background play", cmd_audio_pause),
	SHELL_CMD(resume, NULL, "resume background play", cmd_audio_resume),
	SHELL_CMD(stop, NULL, "stop background play", cmd_audio_stop),
	SHELL_CMD(state, NULL, "session state", cmd_audio_state),
	SHELL_CMD_ARG(seek, NULL, "seek background play <msec>", cmd_audio_seek, 2, 0),
	SHELL_CMD_ARG(loop, NULL, "record then play [ms]", cmd_audio_loop, 1, 1),
	SHELL_SUBCMD_SET_END
);
#endif

/* ---------- wifi ---------- */

#if IS_ENABLED(CONFIG_ASTER_WIFI)
static int cmd_wifi_scan(const struct shell *sh, size_t argc, char **argv)
{
	aster_wifi_ap_t aps[ASTER_WIFI_AP_MAX];
	int n = 0;
	int err;
	int i;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "scanning ...");
	err = aster_plat_wifi_scan(aps, ASTER_WIFI_AP_MAX, &n);
	if (err) {
		shell_print(sh, "scan failed (%d)", err);
		return 0;
	}
	if (n == 0) {
		shell_print(sh, "no APs");
		return 0;
	}
	shell_print(sh, "  ch  rssi  sec              ssid");
	for (i = 0; i < n; i++) {
		shell_print(sh, "  %2u  %4d  %-16s %s",
			    (unsigned)aps[i].channel, (int)aps[i].rssi,
			    aster_plat_wifi_sec_txt(aps[i].security),
			    aps[i].ssid);
	}
	shell_print(sh, "%d AP%s", n, n == 1 ? "" : "s");
	return 0;
}

static int cmd_wifi_connect(const struct shell *sh, size_t argc, char **argv)
{
	const char *psk = (argc >= 3) ? argv[2] : NULL;
	int err;

	if (argc < 2) {
		shell_error(sh, "usage: hw wifi connect <ssid> [psk]");
		return -EINVAL;
	}
	shell_print(sh, "connecting %s ...", argv[1]);
	err = aster_plat_wifi_connect(argv[1], psk);
	if (err) {
		shell_print(sh, "connect failed (%d)", err);
		return 0;
	}
	{
		aster_wifi_status_t st;

		if (aster_plat_wifi_status(&st) == 0) {
			shell_print(sh, "connected ssid=%s ip=%s rssi=%d %s",
				    st.ssid[0] ? st.ssid : argv[1],
				    st.ip[0] ? st.ip : "-",
				    (int)st.rssi,
				    st.state_txt ? st.state_txt : "");
		} else {
			shell_print(sh, "connected");
		}
	}
	return 0;
}

static int cmd_wifi_disconnect(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	err = aster_plat_wifi_disconnect();
	shell_print(sh, err ? "disconnect failed (%d)" : "disconnected", err);
	return 0;
}

static int cmd_wifi_status(const struct shell *sh, size_t argc, char **argv)
{
	aster_wifi_status_t st;
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	err = aster_plat_wifi_status(&st);
	if (err) {
		shell_print(sh, "status failed (%d) state=%s", err,
			    st.state_txt ? st.state_txt : "-");
		return 0;
	}
	shell_print(sh, "state=%s ssid=%s ip=%s rssi=%d sec=%s",
		    st.state_txt ? st.state_txt : "-",
		    st.ssid[0] ? st.ssid : "-",
		    st.ip[0] ? st.ip : "-",
		    (int)st.rssi,
		    st.sec_txt ? st.sec_txt : "-");
    uint32_t operation, last_operation;
    int result;
    aster_plat_wifi_operation(&operation, &last_operation, &result);
    shell_print(sh, "operation=%u last_operation=%u last_result=%d", operation, last_operation, result);
	return 0;
}

static int cmd_wifi_reconnect(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "queueing saved-network reconnect ...");
	err = aster_plat_wifi_reconnect_start();
	if (err) {
		shell_print(sh, "reconnect failed (%d)", err);
	} else {
		shell_print(sh, "reconnect queued; query hw wifi status");
	}
	return 0;
}

static int cmd_wifi_forget(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	err = aster_plat_wifi_forget();
	if (err) {
		shell_print(sh, "forget failed (%d)", err);
	} else {
		shell_print(sh, "saved Wi-Fi profile forgotten");
	}
	return 0;
}

static int cmd_wifi_saved(const struct shell *sh, size_t argc, char **argv)
{
	char ssid[ASTER_WIFI_SSID_MAX + 1];
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	err = aster_plat_wifi_last_ssid(ssid, sizeof(ssid));
	if (err) {
		shell_print(sh, "no saved network (%d)", err);
	} else {
		shell_print(sh, "saved network: %s", ssid);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_wifi,
	SHELL_CMD(scan, NULL, "scan nearby APs", cmd_wifi_scan),
	SHELL_CMD_ARG(connect, NULL, "join <ssid> [psk]", cmd_wifi_connect, 2, 1),
	SHELL_CMD(disconnect, NULL, "leave current AP", cmd_wifi_disconnect),
	SHELL_CMD(status, NULL, "station status", cmd_wifi_status),
	SHELL_CMD(reconnect, NULL, "rejoin saved AP", cmd_wifi_reconnect),
	SHELL_CMD(saved, NULL, "show saved AP name", cmd_wifi_saved),
	SHELL_CMD(forget, NULL, "erase saved AP credentials", cmd_wifi_forget),
	SHELL_SUBCMD_SET_END
);
#endif

/* ---------- image viewer (imgdec) ---------- */
#if IS_ENABLED(CONFIG_ASTER_IMGDEC_SHELL)

static bool img_nn_arg(size_t argc, char **argv, size_t path_idx)
{
	for (size_t i = path_idx + 1; i < argc; i++) {
		if (strcmp(argv[i], "--nn") == 0) {
			return true;
		}
	}
	return false;
}

static int cmd_img_info(const struct shell *sh, size_t argc, char **argv)
{
	if (!aster_storage_mounted()) { shell_error(sh, "SD not mounted"); return -ENODEV; }
	aster_display_info_t d;
	/* No fabricated fallback geometry: whatever is written here would be a
	 * portrait literal baked into a tool that has to work in both
	 * orientations, and a wrong preview size is silent. */
	if (aster_plat_display_get_info(0, &d) != ASTER_OK) {
		shell_error(sh, "no display");
		return -ENODEV;
	}
	aster_imgdec_info_t in;
	aster_error_t rc = aster_imgdec_probe(argv[1], &in, d.width, d.height);
	shell_print(sh, "%s fmt=%s src=%ux%u file=%u B -> preview %ux%u",
		    rc == ASTER_OK ? "ok" : "note", aster_imgdec_fmt_name(in.fmt),
		    in.src_w, in.src_h, in.file_size, in.out_w, in.out_h);
	if (in.unsupported) shell_warn(sh, "decode unsupported: %s", in.why);
	else if (in.too_big) shell_warn(sh, "too big: %s", in.why);
	if (!in.unsupported && !in.too_big)
		shell_print(sh, "ready: hw img show %s", argv[1]);
	return 0;
}

static int cmd_img_show(const struct shell *sh, size_t argc, char **argv)
{
	if (!aster_storage_mounted()) { shell_error(sh, "SD not mounted"); return -ENODEV; }
	bool nn = img_nn_arg(argc, argv, 1);
	int64_t t0 = k_uptime_get();
	aster_error_t rc = aster_imgdec_show(argv[1], nn);
	int64_t dt = k_uptime_get() - t0;
	if (rc == ASTER_ENOTSUP) shell_error(sh, "format not decodable yet (%s)", argv[1]);
	else if (rc == ASTER_ENOMEM) shell_error(sh, "image too large to decode");
	else if (rc != ASTER_OK) shell_error(sh, "show failed (%d)", (int)rc);
	else shell_print(sh, "shown in %lld ms (%s)", dt, nn ? "nearest" : "bilinear");
	return 0;
}

/* Segment the work: probe / render (seek+read+scale) / present (blit). */
static int cmd_img_bench(const struct shell *sh, size_t argc, char **argv)
{
	if (!aster_storage_mounted()) { shell_error(sh, "SD not mounted"); return -ENODEV; }
	bool nn = img_nn_arg(argc, argv, 1);
	aster_display_info_t d;
	/* No fabricated fallback geometry: whatever is written here would be a
	 * portrait literal baked into a tool that has to work in both
	 * orientations, and a wrong preview size is silent. */
	if (aster_plat_display_get_info(0, &d) != ASTER_OK) {
		shell_error(sh, "no display");
		return -ENODEV;
	}

	aster_imgdec_info_t in;
	int64_t t0 = k_uptime_get();
	aster_error_t rc = aster_imgdec_probe(argv[1], &in, d.width, d.height);
	int64_t t_probe = k_uptime_get() - t0;
	if (in.unsupported || in.too_big) { shell_error(sh, "not decodable: %s", in.why); return 0; }

	uint8_t *canvas = aster_psram_alloc((size_t)in.out_w * in.out_h * 2);
	if (!canvas) { shell_error(sh, "no PSRAM for canvas"); return -ENOMEM; }
	uint32_t ow, oh;
	t0 = k_uptime_get();
	rc = aster_imgdec_render(argv[1], canvas, in.out_w, in.out_h, &ow, &oh, nn);
	int64_t t_render = k_uptime_get() - t0;
	if (rc != ASTER_OK) { aster_psram_free(canvas); shell_error(sh, "render %d", (int)rc); return 0; }

	t0 = k_uptime_get();
	aster_plat_display_clear(0x0000);
	uint32_t x = (d.width > ow) ? (d.width - ow) / 2 : 0;
	uint32_t y = (d.height > oh) ? (d.height - oh) / 2 : 0;
	aster_plat_display_write_at(canvas, x, y, ow, oh, ow);
	aster_plat_display_flush_drain();   /* must finish before free (async UAF) */
	int64_t t_present = k_uptime_get() - t0;
	aster_psram_free(canvas);

	shell_print(sh, "%s %ux%u -> %ux%u (%s)", aster_imgdec_fmt_name(in.fmt),
		    in.src_w, in.src_h, ow, oh, nn ? "nearest" : "bilinear");
	shell_print(sh, "probe=%lld ms  render=%lld ms  present=%lld ms  total=%lld ms",
		    t_probe, t_render, t_present, t_probe + t_render + t_present);
	return 0;
}

/* Print the raw RGB565 of one decoded output row as colour-run tokens, so a
 * host can diff the device decoder against a reference without a camera. */
static int cmd_img_dump(const struct shell *sh, size_t argc, char **argv)
{
	if (!aster_storage_mounted()) { shell_error(sh, "SD not mounted"); return -ENODEV; }
	aster_display_info_t d;
	/* No fabricated fallback geometry: whatever is written here would be a
	 * portrait literal baked into a tool that has to work in both
	 * orientations, and a wrong preview size is silent. */
	if (aster_plat_display_get_info(0, &d) != ASTER_OK) {
		shell_error(sh, "no display");
		return -ENODEV;
	}
	aster_imgdec_info_t in;
	aster_error_t rc = aster_imgdec_probe(argv[1], &in, d.width, d.height);
	if (in.unsupported || in.too_big) { shell_error(sh, "not decodable: %s", in.why); return 0; }
	uint32_t oy = (uint32_t)strtoul(argv[2], NULL, 10);
	bool nn = argc > 3 && strcmp(argv[3], "--nn") == 0;

	uint8_t *canvas = aster_psram_alloc((size_t)in.out_w * in.out_h * 2);
	if (!canvas) { shell_error(sh, "no PSRAM"); return -ENOMEM; }
	uint32_t ow, oh;
	rc = aster_imgdec_render(argv[1], canvas, in.out_w, in.out_h, &ow, &oh, nn);
	if (rc != ASTER_OK) { aster_psram_free(canvas); shell_error(sh, "render %d", (int)rc); return 0; }
	if (oy >= oh) { shell_error(sh, "row %u >= oh %u", oy, oh); aster_psram_free(canvas); return 0; }

	const uint16_t *row = (const uint16_t *)canvas + (size_t)oy * ow;
	shell_print(sh, "dump %s oy=%u ow=%u oh=%u", aster_imgdec_fmt_name(in.fmt), oy, ow, oh);
	/* emit as compact hex so the host parses exactly */
	for (uint32_t x = 0; x < ow; x++) {
		shell_fprintf(sh, SHELL_NORMAL, "%04x", row[x]);
		if (x + 1 < ow) {
			shell_fprintf(sh, SHELL_NORMAL, " ");
		}
	}
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	aster_psram_free(canvas);
	return 0;
}

/* raw byte window as contiguous hex (for xxd -r -p on the host). Diagnostic
 * only: pulls a file slice off the SD so a host can reproduce a decode bug
 * on the exact bytes. `hw img hex <path> <off> <len>`. */
static int cmd_img_hex(const struct shell *sh, size_t argc, char **argv)
{
	if (!aster_storage_mounted()) { shell_error(sh, "SD not mounted"); return -ENODEV; }
	off_t off = (off_t)strtoull(argv[2], NULL, 0);
	size_t len = strtoull(argv[3], NULL, 0);
	if (len > 0x400000) len = 0x400000;
	struct fs_file_t f; fs_file_t_init(&f);
	if (fs_open(&f, argv[1], FS_O_READ) != 0) { shell_error(sh, "open"); return -ENOENT; }
	if (fs_seek(&f, off, FS_SEEK_SET) != 0) { fs_close(&f); shell_error(sh, "seek"); return -EIO; }
	uint8_t buf[256];
	shell_print(sh, "#HEX %s %lld %u", argv[1], (long long)off, (unsigned)len);
	size_t done = 0;
	while (done < len) {
		size_t want = len - done < sizeof(buf) ? len - done : sizeof(buf);
		int r = aster_fs_read_safe(&f, buf, want);
		if (r <= 0) break;
		for (int i = 0; i < r; i++) {
			shell_fprintf(sh, SHELL_NORMAL, "%02x", buf[i]);
			if ((i & 15) == 15) shell_fprintf(sh, SHELL_NORMAL, "\n");
		}
		shell_fprintf(sh, SHELL_NORMAL, "\n");
		done += (size_t)r;
	}
	shell_print(sh, "#END");
	fs_close(&f);
	return 0;
}

/* ---- hw img view: minimal directory slideshow ----
 * Swipe left/right = next/prev image, BOOT = quit. Rendering reuses
 * aster_imgdec_show (fit-to-screen, streaming decoder). This blocks the shell
 * thread until BOOT; nothing else should be driving input meanwhile.
 * Note: FAT readdir order is directory order, not alphabetical. */
#define VIEW_MAX   64
#define VIEW_PATH  128
#define VIEW_SWIPE 30   /* min |dx| logical px to call a horizontal swipe */

static bool ci_eq(const char *a, const char *b)
{
	while (*a && *b) {
		char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
		if (ca != cb) return false;
		a++; b++;
	}
	return !*a && !*b;
}

static bool img_is_image(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot) return false;
	dot++;
	return ci_eq(dot, "bmp") || ci_eq(dot, "jpg") ||
	       ci_eq(dot, "jpeg") || ci_eq(dot, "png");
}

static int cmd_img_view(const struct shell *sh, size_t argc, char **argv)
{
	if (!aster_storage_mounted()) { shell_error(sh, "SD not mounted"); return -ENODEV; }
	const char *dir = argv[1];
	bool nn = img_nn_arg(argc, argv, 1);

	/* shell-thread command; 8 KB static beats blowing the stack */
	static char names[VIEW_MAX][VIEW_PATH] __attribute__((section(".ext_ram.bss.hw_cmd_names")));
	int cnt = 0, skipped = 0;
	struct fs_dir_t dp; fs_dir_t_init(&dp);
	if (fs_opendir(&dp, dir) != 0) { shell_error(sh, "opendir %s", dir); return -ENOENT; }
	for (;;) {
		struct fs_dirent de;
		if (fs_readdir(&dp, &de) != 0 || de.name[0] == '\0') break;
		if (de.type != FS_DIR_ENTRY_FILE || !img_is_image(de.name)) continue;
		if (cnt == VIEW_MAX) { skipped++; continue; }
		char leaf[VIEW_PATH - 8];
		strncpy(leaf, de.name, sizeof(leaf) - 1);
		leaf[sizeof(leaf) - 1] = 0;
		snprintf(names[cnt], VIEW_PATH, "%s/%s", dir, leaf);
		cnt++;
	}
	fs_closedir(&dp);
	if (cnt == 0) { shell_error(sh, "no images in %s", dir); return -ENOENT; }
	if (skipped) shell_warn(sh, "only first %d of %d images", VIEW_MAX, VIEW_MAX + skipped);

	if (!aster_plat_input_touch_available()) shell_warn(sh, "no touch: BOOT only");
	shell_print(sh, "view %s: %d images -- swipe L/R switch, BOOT quit", dir, cnt);
	int idx = 0;
	for (;;) {
		const char *base = strrchr(names[idx], '/') ;
		base = base ? base + 1 : names[idx];
		aster_error_t rc = aster_imgdec_show(names[idx], nn);
		if (rc == ASTER_OK) shell_print(sh, "[%d/%d] %s shown", idx + 1, cnt, base);
		else shell_print(sh, "[%d/%d] %s failed (%d)", idx + 1, cnt, base, (int)rc);

		/* drop edges that accumulated during the (slow) render */
		aster_touch_evt_t tev[ASTER_PLAT_MAX_TOUCH];
		while (aster_plat_input_poll(tev, ASTER_PLAT_MAX_TOUCH) > 0) {
		}
		aster_key_evt_t kev[ASTER_PLAT_MAX_KEYS];
		while (aster_plat_input_keys_poll(kev, ASTER_PLAT_MAX_KEYS) > 0) {
		}

		bool pressed = false;
		uint32_t px = 0;
		for (;;) {   /* input loop until swipe or BOOT */
			int n = aster_plat_input_poll(tev, ASTER_PLAT_MAX_TOUCH);
			for (int i = 0; i < n; i++) {
				if (tev[i].action == ASTER_TOUCH_DOWN) {
					px = tev[i].x; pressed = true;
				} else if (tev[i].action == ASTER_TOUCH_UP && pressed) {
					int32_t dx = (int32_t)tev[i].x - (int32_t)px;
					if (dx >= VIEW_SWIPE)  idx = (idx + cnt - 1) % cnt;
					else if (dx <= -VIEW_SWIPE) idx = (idx + 1) % cnt;
					else continue;
					goto next_image;
				}
			}
			int m = aster_plat_input_keys_poll(kev, ASTER_PLAT_MAX_KEYS);
			for (int i = 0; i < m; i++) {
				if (kev[i].code == INPUT_KEY_0 && kev[i].pressed) {
					shell_print(sh, "view: quit at [%d/%d]", idx + 1, cnt);
					return 0;
				}
			}
			k_msleep(10);
		}
next_image:;
	}
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_img,
	SHELL_CMD_ARG(info, NULL, "probe <path>", cmd_img_info, 2, 0),
	SHELL_CMD_ARG(show, NULL, "show <path> [--nn]", cmd_img_show, 2, 1),
	SHELL_CMD_ARG(view, NULL, "view <dir> [--nn]", cmd_img_view, 2, 1),
	SHELL_CMD_ARG(bench, NULL, "bench <path> [--nn]", cmd_img_bench, 2, 1),
	SHELL_CMD_ARG(dump, NULL, "dump <path> <row> [--nn]", cmd_img_dump, 3, 1),
	SHELL_CMD_ARG(hex, NULL, "hex <path> <off> <len>", cmd_img_hex, 4, 0),
	SHELL_SUBCMD_SET_END
);
#endif /* CONFIG_ASTER_IMGDEC_SHELL */

/* ---------- tree ---------- */

SHELL_STATIC_SUBCMD_SET_CREATE(hw_cmds,
	SHELL_CMD(backlight, NULL, "on|off", cmd_bl),
	SHELL_CMD(blpulse, NULL, "blink test", cmd_bl_pulse),
	SHELL_CMD(display, &sub_disp, "display ops", NULL),
	SHELL_CMD_ARG(touch, NULL, "poll/inject touch [n | tap <x> <y> | down|move <x> <y>"
		      " | up | slot <0|1> <down|move|up> <x> <y> | state"
		      " | drag <x1> <y1> <x2> <y2> [steps] [step_ms]]",
		      cmd_touch, 1, 7),
	SHELL_CMD_ARG(key, NULL, "poll/inject keys [inject <code> down|up]", cmd_key, 1, 3),
	SHELL_CMD(imu, NULL, "read IMU", cmd_imu),
#if IS_ENABLED(CONFIG_ASTER_CAMERA)
	SHELL_CMD(cam, &sub_cam, "camera", NULL),
#endif
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
	SHELL_CMD(audio, &sub_audio, "audio play/record/volume", NULL),
#endif
#if IS_ENABLED(CONFIG_ASTER_WIFI)
	SHELL_CMD(wifi, &sub_wifi, "wifi scan/join/status", NULL),
#endif
#if IS_ENABLED(CONFIG_ASTER_IMGDEC_SHELL)
	SHELL_CMD(img, &sub_img, "image decode/preview (BMP/JPG/PNG)", NULL),
#endif
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(hw, &hw_cmds, "raw hardware control", NULL);
