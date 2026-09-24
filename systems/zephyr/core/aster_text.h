/* SPDX-License-Identifier: MIT
 * UTF-8 decoding shared by the text paths (ui_gfx.c, font_sd.c).
 *
 * freestanding on purpose: pp_gfx.c compiles under a host compiler for the
 * preview tool (firmware/ui/hostpreview/preview.c) with no Zephyr headers
 * around, so this file must only ever include <stdint.h>.
 */
#ifndef ASTER_TEXT_H
#define ASTER_TEXT_H

#include <stdint.h>

/* Decode one codepoint from *p. Illegal sequences return 0xFFFD and advance
 * one byte. Always advances at least one byte. */
/* (One exception, kept from the two implementations this merges: a 0x00 byte
 * returns 0 and does NOT advance. Both callers stop on 0, so a "fix" here
 * would only turn their early exit into an infinite loop.) */
/* NOTE: the continuation bytes are read at s[1..3] without an end check --
 * callers guarantee the string is NUL-terminated, and that contract is part
 * of the behaviour being preserved. */
static inline uint32_t aster_utf8_next(const uint8_t **p)
{
	const uint8_t *s = *p;
	uint8_t ch = s[0];
	uint32_t cp, extra, i;

	if (ch == 0u) {
		return 0;
	}
	if (ch < 0x80u) {
		*p = s + 1;
		return ch;
	}
	if ((ch & 0xE0u) == 0xC0u) {
		cp = ch & 0x1Fu;
		extra = 1;
	} else if ((ch & 0xF0u) == 0xE0u) {
		cp = ch & 0xFu;
		extra = 2;
	} else if ((ch & 0xF8u) == 0xF0u) {
		cp = ch & 7u;
		extra = 3;
	} else {
		*p = s + 1;
		return 0xFFFD;
	}
	for (i = 0; i < extra; i++) {
		if ((s[1 + i] & 0xC0u) != 0x80u) {
			*p = s + 1;
			return 0xFFFD;
		}
		cp = (cp << 6) | (s[1 + i] & 0x3Fu);
	}
	*p = s + 1 + extra;
	return cp;
}

#endif /* ASTER_TEXT_H */
