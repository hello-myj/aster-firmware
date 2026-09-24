/* SPDX-License-Identifier: MIT
 * ABI helpers: pixel format conversion.
 */
#ifndef ASTER_UTIL_H
#define ASTER_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <aster/types.h>

/* The one list of pixel formats the firmware can put on the glass.
 *
 * Three files used to carry their own hand-written copy of this list -- the
 * converter switch in aster_util.c, the surface gate in runtime/aster_native.c,
 * and the supported_formats bitmask in platform/<port>/display.c -- and they had
 * already drifted. RGB888 was implemented in the first and accepted in the
 * second, but named in neither of the other two, so an app that polled
 * supported_formats for it was told "not supported" about a path that
 * existed. Deriving all three from this table is what makes that class of
 * drift unrepresentable rather than merely fixed: a format is either in this
 * list or it is nowhere, and every consumer reads the same rows.
 *
 * Columns: X(format, bytes per pixel, row converter -- defined in
 * aster_util.c, named here only so the table reads as a list of real
 * functions; consumers that only want the bits never expand the third
 * column).
 *
 * The list is port-independent on purpose: conversion is done in software,
 * so every board that can receive RGB565 can receive all of these. A board
 * that genuinely cannot would be the first reason to make this conditional,
 * and it should then be a per-board *addition* to a common base rather than
 * a second copy of the list.
 *
 * Note on the shape of the set: ASTER_FMT_BIT() maps a format to a bit through
 * its low six bits, so formats differing only in the "family" nibble collide
 * (RGB565/BGR565 share bit 2; the four 32-bit formats share bit 4). For a
 * mask derived from *this* table that is lossy but never wrong -- when a bit
 * is set, every format mapping to it really is convertible, because the
 * whole table is. It would become a false positive only for a board
 * advertising a proper subset of these rows; see ASTER_FORMATS_CONVERTIBLE. */
#define ASTER_FORMAT_TABLE(X)					\
	X(ASTER_PIXEL_RGB565,   2, aster_row_rgb565)		\
	X(ASTER_PIXEL_BGR565,   2, aster_row_bgr565)		\
	X(ASTER_PIXEL_RGB888,   3, aster_row_rgb888)		\
	X(ASTER_PIXEL_RGBA8888, 4, aster_row_rgba8888)		\
	X(ASTER_PIXEL_BGRA8888, 4, aster_row_bgra8888)

/* One bit per row, concatenated: the set the firmware can actually deliver.
 * This is what display.c advertises as
 * aster_display_info_t::supported_formats, so "what we claim" and "what we
 * convert" are the same expression rather than two lists that agree today.
 *
 * When reading it as a bitmask, remember what a set bit means: since every
 * bit is derived here, a set bit always implies the conversion exists (see
 * the collision note above). It does NOT identify a single format -- an app
 * asking about BGR565 is told yes because RGB565 and BGR565 share a bit, and
 * on this firmware that answer happens to be true. If a board ever needs to
 * advertise a strict subset of these rows, the bit rule has to be tightened
 * in the ABI first, or that board will start claiming formats it cannot
 * convert. */
#define ASTER_FORMAT_BIT(fmt_, bpp_, fn_) (ASTER_FMT_BIT(fmt_)) |
#define ASTER_FORMATS_CONVERTIBLE (ASTER_FORMAT_TABLE(ASTER_FORMAT_BIT) 0)

/* RGB565 (LE u16) from 8888 components */
static inline uint16_t aster_rgb888_to_565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Convert a pixel buffer of `fmt` to RGB565. src has width*height pixels at
 * src pitch (bytes/row); dst is width*height u16 rows packed. Accepts exactly
 * the formats in ASTER_FORMAT_TABLE; anything else returns ASTER_ENOTSUP. */
int aster_convert_to_rgb565(const uint8_t *src, uint32_t src_pitch, uint32_t fmt,
			  uint16_t *dst, uint32_t dst_pitch, uint32_t w, uint32_t h);

#endif
