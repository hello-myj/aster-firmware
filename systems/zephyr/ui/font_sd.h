/* SPDX-License-Identifier: MIT
 * Full-coverage cpfont resources on SD. Header/interval metadata stays
 * resident; each used size lazily allocates its own LRU glyph cache in PSRAM.
 * Prewarm before presentation, release on screen-owner changes. Font objects
 * remain stable while their glyph storage is released. All calls use the
 * shared WAMR/LVGL worker, including session teardown.
 */
#ifndef ASTER_FONT_SD_H
#define ASTER_FONT_SD_H

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

#include <aster/font_metrics.h>

/* Baked size #i in bake order, 0 when i is past the end, and how many
 * there are. Both are pure: they read a compile-time table, so they are
 * safe before aster_font_sd_init() and are what callers iterate instead of
 * keeping a size list of their own. */
uint32_t aster_font_sd_face_n(void);
uint8_t aster_font_sd_face_px(uint32_t i);

/* Discover SD face headers and interval metadata, retrying missing files.
 * Returns 1 when a new native face becomes available (caller repaints).
 * UI thread only. */
int aster_font_sd_poll(void);

/* Legacy progress hooks: prewarm is synchronous, so no background queue remains. */
bool aster_font_sd_streaming(void);
bool aster_font_sd_busy(void);
uint32_t aster_font_sd_progress(void);

/* cpfont-backed font for `px`, or NULL when `px` is not a baked size, its
 * file is absent, or cache allocation fails. The
 * pointer is stable for the slot lifetime. */
const lv_font_t *aster_font_sd_get(uint8_t px);

/* Preload UTF-8 glyphs into this size cache. Cold glyphs read SD synchronously;
 * prewarm during boot/init to avoid cold I/O during normal drawing. */
void aster_font_sd_warm(uint8_t px, const char *utf8);

/* Shared UI worker only: one lazy file handle for a bounded synchronous
 * operation; nestable, must end before yielding/returning. No I/O on warm hits. */
void aster_font_sd_io_begin(void);
void aster_font_sd_io_end(void);

/* Compatibility hook: synchronous glyph reads have no staged completions. */
bool aster_font_sd_commit(void);

/* Poll newly available SD font metadata; glyphs load in the requesting call. */
bool aster_font_sd_repaint_pending(void);

/* Clear the repaint request. Called by the draw entry point (ui_gfx.c),
 * which is about to paint everything the pipeline has. */
void aster_font_sd_repaint_ack(void);

/* UI thread: monotonic update counter, unaffected by guest repaint ack. */
uint32_t aster_font_sd_generation(void);

/* Codepoint present in the baked set of `px`? (interval lookup, no I/O;
 * false while the slot is not ready.) */
bool aster_font_sd_has_cp(uint8_t px, uint32_t cp);

/* Boot registers all native sizes; poll discovers their interval tables.
 * Glyph caches allocate only when a size is requested. */
void aster_font_sd_init(void);

/* Every APP begins a session before init. Desktop glyph caches are released;
 * requested sizes allocate on demand. End releases APP glyph caches. */
void aster_font_sd_session_begin(void);
void aster_font_sd_session_end(void);

/* Release all size caches when ownership changes; interval metadata stays. */
void aster_font_sd_cache_drop(void);

/* Profiling (CONFIG_ASTER_LVGL_PROFILER builds the printer; counters always
 * maintained): table/metadata reads, warm bursts, misses, white-box draws. */
typedef struct {
	uint32_t reads;         /* SD read calls (incl. table chunks)      */
    uint32_t hits;          /* resident glyph lookups */
    uint32_t load_us;       /* successful cold loads, wrapping sum */
    uint32_t max_load_us;
    /* Wrapping microsecond sums: acquire/record/bitmap for successful loads.
     * Seek is part of the read; close also includes scoped/error cleanup.
     * Failures include open/read/validation/close errors. */
    uint32_t open_us, record_us, bitmap_us, close_us, load_failures;
    uint32_t warm_us;       /* wrapping sum of synchronous warm time */
	uint32_t read_bytes;
	uint32_t warm_bursts;   /* prewarm calls               */
	uint32_t warm_glyphs;   /* glyphs covered by warm bursts           */
	uint32_t misses;        /* cold SD glyph loads       */
	uint32_t miss_filled;   /* successful cold loads       */
	uint32_t evictions;     /* LRU glyph removals                          */
	uint32_t oversize;      /* glyphs dropped: bigger than a loader    */
				/* buffer (SCRATCH_BYTES).       */
				/* Non-zero means holes in the page; the  */
				/* two buffers are bake-derived, so this  */
				/* should never grow except on a bake/card */
				/* mismatch.                              */
} aster_font_sd_stats_t;

void aster_font_sd_stats(aster_font_sd_stats_t *out);

typedef struct {
 uint32_t px,coverage,glyphs,used,bytes;
 bool available;
} aster_font_cache_status_t;
/* Snapshot stored in stable slots; shell never dereferences cache allocations. */
bool aster_font_sd_cache_status(uint32_t index,aster_font_cache_status_t *out);

#endif /* ASTER_FONT_SD_H */
