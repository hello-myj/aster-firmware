/* SPDX-License-Identifier: MIT */
#ifndef ASTER_SYSTEM_H
#define ASTER_SYSTEM_H

#include "types.h"

#define ASTER_ABI_MAJOR 3
#define ASTER_ABI_MINOR 12 /* 3.12 (additive): bounded WebSocket/WSS client.
                          * 3.11 (additive): bounded MQTT/MQTTS client.
                          * 3.10 (additive): bounded MP3 stream input.
                          * 3.9 (additive): bounded HTTP/HTTPS client.
                          * 3.8 (additive): bounded asynchronous TCP/UDP.
                          * 3.7 (additive): coherent six-axis IMU info/read.
                          * 3.6 (additive): read-only wall-clock time,
                          * UTC epoch milliseconds plus local calendar fields.
                          * 3.5 (additive): station Wi-Fi scan/connect/status,
                          * saved profile, forget and reconnect imports.
                          * 3.4 (additive): short effect load/play/unload can
                          * mix PCM effects over a background audio session.
                          * 3.3 (breaking, no API change): the fixed-width
                          * container and wire markers were renamed with the
                          * rest of the MJX-era spellings -- ASTP packages,
                          * ASTI icons, ASTR asset blobs, the ASTERFS transfer
                          * protocol. Nothing in the aster_* API moved, but a
                          * 3.2 device and a 3.3 package do not understand each
                          * other's bytes, so the minor bump is what makes the
                          * mismatch visible at the handshake instead of at the
                          * first parse. 3.2 (additive):
                          * aster_audio_get_info -- the native
                          * PCM format is a per-board fact and must be asked
                          * for; the ASTER_AUDIO_RATE/_CHANNELS/_BITS macros it
                          * replaces are deleted, because they were a board's
                          * numbers in a header compiled into an app that is
                          * installed on any board. 3.1 (additive):
                          * aster_gfx_fonts_pending -- the gfx engine's
                          * async-font repaint signal, so an event-driven app
                          * can tell when a blank box it drew has been filled
                          * in. 3.0 (breaking): the
                          * wasm-side widget runtime is deleted -- ui.h
                          * imports, ASTER_EVENT_UI and the ui.bin package
                          * slot are gone. All pixels, UI or game, now flow
                          * through the gfx command engine (2.2:
                          * exec/text_w/bmp_load). Fonts stay on the 2.1
                          * cpfont + LVGL tiny_ttf stack. */
#define ASTER_ABI_PATCH 0

/* Packed into one u32: 0x00MMmmPP */
#define ASTER_ABI_VERSION \
	(((uint32_t)ASTER_ABI_MAJOR << 16) | ((uint32_t)ASTER_ABI_MINOR << 8) | (uint32_t)ASTER_ABI_PATCH)

/* Feature bits (design doc sec.8): query via aster_system_has_feature() */
#define ASTER_FEATURE_DISPLAY      0u
#define ASTER_FEATURE_TOUCH        1u
#define ASTER_FEATURE_MOUSE        2u
#define ASTER_FEATURE_KEYBOARD     3u
#define ASTER_FEATURE_CAMERA       4u
#define ASTER_FEATURE_AUDIO_INPUT  5u
#define ASTER_FEATURE_AUDIO_OUTPUT 6u
#define ASTER_FEATURE_WIFI         7u
#define ASTER_FEATURE_BLUETOOTH    8u
#define ASTER_FEATURE_ACCELEROMETER 9u
#define ASTER_FEATURE_GYROSCOPE    10u
#define ASTER_FEATURE_FILESYSTEM   11u
#define ASTER_FEATURE_RTC          12u
#define ASTER_FEATURE_PSRAM        13u
#define ASTER_FEATURE_FRAMEBUFFER  14u
#define ASTER_FEATURE_NETWORK      15u
#define ASTER_FEATURE_HTTP         16u
#define ASTER_FEATURE_TLS          17u
#define ASTER_FEATURE_MQTT         19u
#define ASTER_FEATURE_AUDIO_STREAM 18u
#define ASTER_FEATURE_WEBSOCKET    20u

/* CPU arch ids */
#define ASTER_ARCH_XTENSA 1u
#define ASTER_ARCH_ARM    2u
#define ASTER_ARCH_RISCV  3u
#define ASTER_ARCH_X86    4u

typedef struct {
	uint32_t size;    /* set to sizeof(aster_system_info_t) by caller */
	uint32_t version; /* set to 1 by caller */

	uint32_t abi_version; /* ASTER_ABI_VERSION packed */

	char     platform[16]; /* "zephyr", "windows", ... */
	char     board[40];    /* vendor.soc.board style id (board profile) */
	char     soc[24];      /* SoC name as reported by the board profile */

	uint32_t cpu_arch;
	uint32_t cpu_freq_hz;
	uint32_t cpu_count;

	uint32_t ram_internal; /* bytes */
	uint32_t ram_external; /* bytes (PSRAM), 0 if none */
	uint32_t flash_size;   /* bytes */

	uint64_t feature_flags; /* bit = 1<<ASTER_FEATURE_x */
} aster_system_info_t;

/*
 * ABI V1 native surface (import module "env").
 * Apps must NOT call these directly except through the wrappers below.
 *
 * Calling convention notes:
 *  - Every function taking a struct receives the wasm linear-memory pointer
 *    (a u32 offset on wasm32) plus the struct size for forward compat.
 *  - Strings are (offset,len) pairs; len excludes the NUL; the Runtime
 *    validates the full range against the caller's linear memory.
 */
#if defined(__wasm__)
#define ASTER_I(name) __attribute__((import_module("env"), import_name(name)))
#else
#define ASTER_I(name)
#endif

ASTER_I("aster_system_has_feature") int32_t aster_system_has_feature(uint32_t feature_bit);
ASTER_I("aster_system_get_abi_version") uint32_t aster_system_get_abi_version(void);

#endif /* ASTER_SYSTEM_H */
