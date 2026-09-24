#include <aster/aster.h>
#include "aster_sysinfo.h"
/* SPDX-License-Identifier: MIT
 * File-transfer service on the native USB-CDC (Serial-JTAG) port.
 * See aster_file_svc.h for the protocol. Runs on its own polled thread so the
 * CH340 shell stays the interactive control plane.
 *
 * Safety: only paths under /SD:/ are writable, no ".." components; the file
 * is written to <path>.part and rename-published, so a torn transfer can
 * never masquerade as a complete package.
 */
#include "aster_file_svc.h"
#include "aster_storage.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(aster_fsvc, LOG_LEVEL_INF);

#if DT_NODE_HAS_STATUS(DT_NODELABEL(usb_serial), okay)
#define HAVE_CDC 1
#else
#define HAVE_CDC 0
#endif

#define BUF_SZ      512
#define LINE_MAX    192
#define IDLE_MS     5     /* main-loop poll gap */
#define LINE_TMO_MS 30000
#define BODY_TMO_MS 30000

static const struct device *const cdc =
	DEVICE_DT_GET_OR_NULL(DT_NODELABEL(usb_serial));

static K_THREAD_STACK_DEFINE(fsvc_stack, 4096);
static struct k_thread fsvc_thread;

/* .bss -> internal SRAM: fs_write DMA lands fine without a bounce */
static uint8_t buf[BUF_SZ];

static void cdc_puts(const char *s)
{
	while (*s) {
		uart_poll_out(cdc, (unsigned char)*s++);
	}
}

static void cdc_reply(const char *fmt, ...)
{
	char out[64];
	va_list ap;

	va_start(ap, fmt);
	int w = vsnprintf(out, sizeof(out), fmt, ap);
	va_end(ap);
	if (w > 0) {
		cdc_puts(out);
		cdc_puts("\r\n");
	}
}

/* Every command line begins with CMD_PREFIX. The tokens are built from it by
 * concatenation rather than written out a second time: the reader below keeps a
 * byte only while it still continues CMD_PREFIX, so a token that does not
 * actually start with the prefix is discarded before the dispatcher can see it
 * -- dropped instead of answered, with no reply and no error. Spelling the two
 * independently is what let them drift apart once; this makes that
 * unrepresentable. */
#define CMD_PREFIX "ASTER"
#define CMD_PING   CMD_PREFIX "PING"  /* "ASTERPING" */
#define CMD_PUT    CMD_PREFIX "PUT "  /* "ASTERPUT " */

/* Bytes discarded since the last completed line: a client that pipelined a
 * body it should have held back, or one that died mid-line, leaves debris here
 * and the next command is answered out of a channel that was resynchronised
 * rather than poisoned. Reported once per line, not per byte. */
static uint32_t s_line_drops;

/* one \n-terminated line (bare \n tolerable); -1 on timeout/overflow.
 *
 * The reader keeps the buffer only while it can still become a command: until
 * CMD_PREFIX has matched, any byte that does not continue it is discarded on
 * the spot. That is the difference between a client that pipelines and a
 * channel that dies -- measured with aster_negtest's B2 cases (a rejected
 * ASTERPUT whose body was sent anyway): the leftover "abcd" used to sit in the
 * buffer and GLUE itself to the next command, so "abcdASTERPING" arrived as one
 * line,
 * was answered -EINVAL, and the reply the host was actually waiting for came
 * up to LINE_TMO_MS later -- 30 s, indistinguishable from a hang.
 *
 * This does NOT mean the device drains a body: it cannot know whether one was
 * sent (the gated host sends none), and draining a body that never came would
 * eat the next command instead. The protocol stays stop-and-wait; what changed
 * is that a client breaking it now costs its own noise and nothing else. */
static int cdc_read_line(char *line, size_t max, int64_t timeout_ms)
{
	size_t n = 0;
	int64_t t0 = k_uptime_get();

	for (;;) {
		unsigned char c;
		if (uart_poll_in(cdc, &c) == 0) {
			if (c == '\n') {
				if (n > 0 && line[n - 1] == '\r') {
					n--;
				}
				line[n] = 0;
				if (s_line_drops) {
					LOG_WRN("fsvc: dropped %u byte(s) that "
						"cannot start a command (a "
						"pipelined body, or a host not "
						"speaking ASTERFS)", s_line_drops);
					s_line_drops = 0;
				}
				return (int)n;
			}
			if (n < sizeof(CMD_PREFIX) - 1 &&
			    c != (unsigned char)CMD_PREFIX[n]) {
				s_line_drops++;
				continue;
			}
			if (n >= max - 1) {
				return -1;
			}
			line[n++] = (char)c;
			t0 = k_uptime_get();
		} else {
			if (k_uptime_get() - t0 > timeout_ms) {
				return -1;
			}
			k_msleep(1);
		}
	}
}

static int path_ok(const char *p)
{
	if (strncmp(p, ASTER_MOUNT_POINT "/", strlen(ASTER_MOUNT_POINT) + 1) != 0) {
		return -1; /* volume-rooted only */
	}
	if (strstr(p, "..")) {
		return -1;
	}
	size_t l = strlen(p);
	return (l > 0 && l < 160) ? 0 : -1;
}

/* CRC32 of an on-disk file (whole content), or -errno. Used to validate a
 * resumed transfer whose bytes were written across several sessions, where an
 * incremental crc in RAM is impossible. */
static long file_crc32(const char *p, uint32_t *out)
{
	struct fs_file_t f;
	uint32_t crc = 0;
	int r;

	fs_file_t_init(&f);
	r = fs_open(&f, p, FS_O_READ);
	if (r) {
		return r;
	}
	for (;;) {
		int n = fs_read(&f, buf, BUF_SZ);
		if (n < 0) {
			fs_close(&f);
			return n;
		}
		if (n == 0) {
			break;
		}
		crc = crc32_ieee_update(crc, buf, (size_t)n);
	}
	fs_close(&f);
	*out = crc;
	return 0;
}

/* Stop-and-wait body pump: read exactly one BUF_SZ chunk, write it, then
 * reply "ACK <cumulative-offset>". The host never sends the next chunk until
 * it sees the ACK, so the CDC RX ring cannot overflow while fs_write blocks
 * (the root cause of the old dropped-byte / CRC-mismatch failures). Returns 0
 * on success (all `want` bytes written), -ETIMEDOUT on host stall, or a write
 * errno. */
static int pump_body(const struct device *dev, struct fs_file_t *f, uint32_t want,
		     uint32_t base_off)
{
	uint32_t cum = base_off;

	while (want) {
		uint32_t chunk = want > BUF_SZ ? BUF_SZ : want;
		uint32_t n = 0;
		int64_t t0 = k_uptime_get();

		while (n < chunk) {
			unsigned char c;

			if (uart_poll_in(dev, &c) == 0) {
				buf[n++] = c;
				t0 = k_uptime_get();
			} else if (k_uptime_get() - t0 > BODY_TMO_MS) {
				return -ETIMEDOUT;
			}
		}
		int w = fs_write(f, buf, n);
		if (w != (int)n) {
			return w < 0 ? w : -EIO;
		}
		cum += n;
		want -= n;
		if (want) {
			cdc_reply("ACK %u", cum); /* ready for next chunk */
		}
	}
	return 0;
}

/* Durable byte count of the partial file (ground truth after a short-write). */
static uint32_t part_size(const char *tmp)
{
	struct fs_dirent st;

	return (fs_stat(tmp, &st) == 0) ? (uint32_t)st.size : 0u;
}

/* Publish a fully-received .part after the whole-file crc checks out. */
static void publish_part(const char *tmp, const char *path, uint32_t len)
{
	int err = fs_rename(tmp, path);

	if (err) {
		(void)fs_unlink(tmp);
		cdc_reply("ERR %d", err);
		return;
	}
	LOG_INF("fsvc: put %s (%u bytes)", path, len);
	cdc_reply("OK");
}

/* Stop-and-wait upload. Header arrives alone; the host sends the first body
 * byte ONLY after this replies a go-token "ACK <off>". Every subsequent
 * BUF_SZ chunk is likewise gated on the previous ACK, so fs_write latency can
 * never overflow the CDC RX ring. Rejections therefore need no body drain.
 *   go:    "ACK <off>"        send chunk @off (off reflects any on-disk .part)
 *   mid:   "ACK <cum>"        after each chunk; "RESUME <n>" on stall/EIO
 *   done:  "OK" | "ERR retry" | "ERR <code>"
 */
static void do_put(const struct device *dev, const char *path, uint32_t len,
		   uint32_t want_crc, uint32_t off)
{
	if (len == 0 || len > 16u * 1024 * 1024) {
		cdc_reply("ERR %d", -EFBIG);
		return;
	}
	if (!aster_storage_mounted()) {
		cdc_reply("ERR %d", -ENODEV);
		return;
	}

	char tmp[192];
	if (strlen(path) + sizeof(".part") > sizeof(tmp)) {
		cdc_reply("ERR %d", -ENAMETOOLONG);
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.part", path);

	/* make sure the parent dir exists (apps ship to the card root: ok) */
	char dir[192];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = 0;
		(void)aster_storage_mkdir_p(dir);
	}

	if (off == 0) {
		(void)fs_unlink(tmp); /* fresh push starts the .part clean */
	} else {
		/* Resume: trust the on-disk .part, not the claimed offset. */
		uint32_t have = part_size(tmp);

		off = off < have ? off : have; /* trust the disk, not the claim */
		if (off >= len) {
			/* body already landed; verify crc and publish. */
			uint32_t crc = 0;
			int cre = file_crc32(tmp, &crc);

			if (cre) {
				/* Can't read our own .part back: a transient SD
				 * glitch, not corrupt data. Keep it and let the
				 * host re-PUT at len so we retry the verify. */
				cdc_reply("RESUME %u", have);
			} else if (crc == want_crc) {
				(void)fs_unlink(path);
				publish_part(tmp, path, len);
			} else {
				(void)fs_unlink(tmp);
				cdc_reply("ERR retry");
			}
			return;
		}
	}

	struct fs_file_t f;
	fs_file_t_init(&f);
	int err = fs_open(&f, tmp, FS_O_WRITE | FS_O_CREATE);
	if (err) {
		cdc_reply("ERR %d", err);
		return;
	}
	if (off && fs_seek(&f, (off_t)off, FS_SEEK_SET)) {
		fs_close(&f);
		(void)fs_unlink(tmp);
		cdc_reply("ERR retry");
		return;
	}

	/* go-token: host begins streaming from `off` (device-truth offset). */
	cdc_reply("ACK %u", off);

	err = pump_body(dev, &f, len - off, off);
	if (err) {
		(void)fs_sync(&f); /* land durable bytes before we report RESUME */
	}
	fs_close(&f);

	if (err) {
		uint32_t durable = part_size(tmp); /* full chunks only */

		if (err == -ETIMEDOUT) {
			LOG_WRN("fsvc: stalled %s at %u/%u", path, durable, len);
		} else {
			LOG_WRN("fsvc: write %s err %d at %u/%u", path, err,
				durable, len);
		}
		cdc_reply("RESUME %u", durable);
		return;
	}

	uint32_t crc = 0;

	if (file_crc32(tmp, &crc) != 0 || crc != want_crc) {
		(void)fs_unlink(tmp); /* torn tail: restart clean next round */
		LOG_WRN("fsvc: crc mismatch %s (%08x/%08x)", path, crc, want_crc);
		cdc_reply("ERR retry");
		return;
	}

	if (fs_stat(path, &(struct fs_dirent){0}) == 0) {
		(void)fs_unlink(path); /* replace existing (fresh push wins) */
	}
	publish_part(tmp, path, len);
}

static void fsvc_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	char line[LINE_MAX];

	cdc_reply("ASTERFS 3 sd=%d", aster_storage_mounted() ? 1 : 0);

	for (;;) {
		int n = cdc_read_line(line, sizeof(line), LINE_TMO_MS);
		if (n < 0) {
			k_msleep(IDLE_MS);
			continue;
		}
		if (strcmp(line, CMD_PING) == 0) {
			aster_system_info_t info;

			aster_sysinfo_get(&info);
			cdc_reply("PONG sd=%d product=aster target=%s abi=%u.%u caps=%llx",
				  aster_storage_mounted() ? 1 : 0, CONFIG_ASTER_TARGET_ID,
				  ASTER_ABI_MAJOR, ASTER_ABI_MINOR,
				  (unsigned long long)info.feature_flags);
		} else if (strncmp(line, CMD_PUT, sizeof(CMD_PUT) - 1) == 0) {
			char path[160];
			uint32_t len = 0, crc = 0, off = 0;
			const char *args = line + (sizeof(CMD_PUT) - 1);
			char *sp1 = strchr(args, ' ');

			if (!sp1) {
				cdc_reply("ERR %d", -EINVAL);
				continue;
			}
			size_t plen = (size_t)(sp1 - args);
			if (plen >= sizeof(path)) {
				cdc_reply("ERR %d", -ENAMETOOLONG);
				continue;
			}
			memcpy(path, args, plen);
			path[plen] = 0;
			char *sp2 = strchr(sp1 + 1, ' ');
			if (!sp2) {
				cdc_reply("ERR %d", -EINVAL);
				continue;
			}
			len = (uint32_t)strtoul(sp1 + 1, NULL, 10);
			crc = (uint32_t)strtoul(sp2 + 1, NULL, 16);
			/* optional 4th field: resume byte offset (ASTERFS 2) */
			char *sp3 = strchr(sp2 + 1, ' ');
			if (sp3) {
				off = (uint32_t)strtoul(sp3 + 1, NULL, 10);
			}
			if (path_ok(path) != 0) {
				/* Host never streams the body until it sees a
				 * go-"ACK", so nothing to drain: reply alone. */
				cdc_reply("ERR %d", -EPERM);
				continue;
			}
			do_put(cdc, path, len, crc, off);
		} else if (line[0]) {
			cdc_reply("ERR %d", -EINVAL);
		}
	}
}

int aster_file_svc_start(void)
{
#if HAVE_CDC
	if (!device_is_ready(cdc)) {
		LOG_WRN("usb_serial CDC device not ready; file svc off");
		return -ENODEV;
	}
	k_thread_create(&fsvc_thread, fsvc_stack, K_THREAD_STACK_SIZEOF(fsvc_stack),
			fsvc_fn, NULL, NULL, NULL, 7, 0, K_NO_WAIT);
	k_thread_name_set(&fsvc_thread, "aster_fsvc");
	LOG_INF("file svc on USB-CDC (ASTERPUT <path> <len> <crc32> [resume-off])");
	return 0;
#else
	return -ENOTSUP;
#endif
}
