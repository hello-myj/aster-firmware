/* SPDX-License-Identifier: MIT */
#ifndef ASTER_TYPES_H
#define ASTER_TYPES_H
#include <stdint.h>

/* All native objects are exposed as opaque 32-bit handles (design doc sec.18).
 * Handle 0 is invalid. High byte encodes the type tag so the Runtime can
 * verify type and detect forged values. The Runtime additionally checks
 * ownership (which app created it) internally. */
typedef uint32_t aster_handle_t;

#define ASTER_HANDLE_INVALID ((aster_handle_t)0)

#define ASTER_HTYPE_SURFACE 0x02000000u
#define ASTER_HTYPE_TIMER    0x06000000u
#define ASTER_HTYPE_FILE     0x04000000u
#define ASTER_HTYPE_DIR      0x07000000u

/* Offset+length string reference in wasm linear memory (design doc sec.60).
 * Native pointers must never cross the ABI.
 * offset == 0 is always invalid (NULL). length == UINT32_MAX (the frozen
 * "id_len or -1" form, docs/abi-v1.md sec.7) selects NUL-terminated mode:
 * offset points at a string whose terminating NUL must lie inside the linear
 * memory. Any other length is an exact byte count, NUL not included; a zero
 * length is rejected (ASTER_EINVAL), not a NUL-terminated request. */
typedef struct {
	uint32_t offset;
	uint32_t length;
} aster_string_t;

/* Pixel formats (design doc sec.9). Value = bpp in low byte, family in high. */
typedef enum {
	ASTER_PIXEL_MONO1 = 0x0101,
	ASTER_PIXEL_GRAY8 = 0x0801,
	ASTER_PIXEL_RGB565 = 0x1002, /* memory order R[15:11] G[10:5] B[4:0], LE u16 */
	ASTER_PIXEL_BGR565 = 0x1102,
	ASTER_PIXEL_RGB888 = 0x1803,
	ASTER_PIXEL_BGR888 = 0x1903,
	ASTER_PIXEL_XRGB8888 = 0x2004,
	ASTER_PIXEL_ARGB8888 = 0x2104,
	ASTER_PIXEL_RGBA8888 = 0x2204,
	ASTER_PIXEL_BGRA8888 = 0x2304,
	ASTER_PIXEL_NATIVE = 0xFFFF, /* resolve to display's best format */
} aster_pixel_format_t;

/* Bitmask helper for supported_formats. Rule: a format F is supported iff
 * (supported_formats & ASTER_FMT_BIT(F)) is non-zero; ASTER_PIXEL_NATIVE has no
 * bit. 64 bits is ample for V1's handful of formats.
 *
 * The index is the format's LOW SIX BITS, so formats differing only in the
 * "family" nibble share one: RGB565 and BGR565 both land on bit 2, and the
 * four 32-bit formats all land on bit 4 (RGB888 alone gets bit 3). A set bit
 * is therefore not proof that the exact format asked about is supported --
 * it is proof that *a* format with those low six bits is. Every current port
 * converts its whole format table, so the two coincide and a set bit is a
 * true answer; a port that advertised a strict subset of that table would
 * need this rule replaced with a collision-free index first. */
#define ASTER_FMT_BIT(f) (1ull << ((f) & 0x3F))

/* Native import declaration macro. On wasm, attach the wasm-linkage
 * attributes so clang emits `import "env" <name>`; on the host these are
 * no-ops (the header is shared with firmware code which only reads types). */
#if defined(__wasm__)
#define ASTER_I(name) __attribute__((import_module("env"), import_name(name)))
#else
#define ASTER_I(name)
#endif

#endif /* ASTER_TYPES_H */
