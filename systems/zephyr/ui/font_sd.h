/* SPDX-License-Identifier: MIT
 * cpfont v4 bitmap-font loader: LVGL lv_font_t backend for pre-baked
 * CrossPoint-format glyph bitmaps streamed from the SD card.
 *
 * Format + mechanism: docs/cpfont-notes.md (port of CrossPoint Reader CJK,
 * MIT, (github.com/CrossPoint-CJK/crosspoint-reader-cjk)). Independent
 * implementation; wire-compatible with official .cpfont files (single
 * style: regular).
 *
 * Per-size slot lifecycle (all SD work sliced so no caller ever blocks):
 *   EMPTY --(UI poll: header+TOC+interval tables resident)--> TABLE --(the
 *   system workqueue streams the compact metric table in 32 KB chunks)-->
 *   READY (metrics pure-RAM from then on). Bitmaps fill a bump+epoch ring
 *   (RING_BYTES 48 KB, RING_SLOTS 512 entries per size) lazily: warm() bursts and
 *   render-time misses are merged into contiguous-index runs and fetched on
 *   the system workqueue (s_lock is never held across SD I/O). A bitmap requested
 *   before it is resident renders as a correct-sized blank box and is
 *   filled + committed on the next aster_font_sd_commit().
 *
 * Thread model: state transitions, ring writes and teardown happen only on
 * the UI thread; the fetch/stream worker runs on the system workqueue
 * guarded by one mutex plus a cancel-sync before any table free. FatFs is
 * REENTRANT so its own state is safe; our slot structs are the ones we lock.
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

/* Flip finished streams to READY + throttled re-probe of missing files.
 * Returns 1 when any slot newly reached READY (caller repaints).
 * UI thread only. */
int aster_font_sd_poll(void);

/* True while at least one slot is still streaming its metric table from
 * SD. Used to hold the launcher's very first paint until the baked fonts
 * can answer (UI thread only). */
bool aster_font_sd_streaming(void);
/* True while a glyph prefetch is queued or the SD worker is still reading
 * bitmaps. The launcher holds the splash until names and descriptions land. */
bool aster_font_sd_busy(void);

/* Boot-splash progress 0..100 across all baked sizes (0 before discovery,
 * 100 when every slot is READY or DEAD). UI thread only. */
uint32_t aster_font_sd_progress(void);

/* cpfont-backed font for `px`, or NULL when `px` is not a baked size, its
 * file is absent, or it has not finished streaming its metric table. The
 * pointer is stable for the slot lifetime. */
const lv_font_t *aster_font_sd_get(uint8_t px);

/* Queue the unique codepoints of `utf8` for async bitmap prefetch at `px`
 * (runs merged into single burst reads). Never blocks; drops when full. */
void aster_font_sd_warm(uint8_t px, const char *utf8);

/* Drain worker completions into the bitmap rings. Returns 1 when new
 * bitmaps landed (caller repaints the screen). UI thread only. */
bool aster_font_sd_commit(void);

/* "Repaint now?" for a guest that does not pump this module itself.
 * Advances poll+commit (a non-drawing app would otherwise strand fills in
 * staging) and returns true when something landed or a slot came READY.
 * Reading is idempotent: the request is cleared by the draw (ack below),
 * not by the read, so asking twice in a frame cannot swallow the signal. */
bool aster_font_sd_repaint_pending(void);

/* Clear the repaint request. Called by the draw entry point (ui_gfx.c),
 * which is about to paint everything the pipeline has. */
void aster_font_sd_repaint_ack(void);

/* UI thread: monotonic update counter, unaffected by guest repaint ack. */
uint32_t aster_font_sd_generation(void);

/* Codepoint present in the baked set of `px`? (interval lookup, no I/O;
 * false while the slot is not ready.) */
bool aster_font_sd_has_cp(uint8_t px, uint32_t cp);

/* Boot-time hook: registers the baked sizes and loads only the interval
 * tables. Metric tables and bitmap rings stay unloaded until a reading
 * session asks for them. */
void aster_font_sd_init(void);

/* Reader, lyrics and chat call this on the UI thread as they start. The
 * first call allocates the metric tables and rings; the matching end
 * releases them. Nested calls are counted. */
void aster_font_sd_session_begin(void);
void aster_font_sd_session_end(void);

/* Drop the small on-demand glyph cache. Pages that draw SSIDs or file
 * names call this as they close. */
void aster_font_sd_cache_drop(void);

/* Profiling (CONFIG_ASTER_LVGL_PROFILER builds the printer; counters always
 * maintained): table/metadata reads, warm bursts, misses, white-box draws. */
typedef struct {
	uint32_t reads;         /* SD read calls (incl. table chunks)      */
	uint32_t read_bytes;
	uint32_t warm_bursts;   /* merged burst reads served               */
	uint32_t warm_glyphs;   /* glyphs covered by warm bursts           */
	uint32_t misses;        /* on-demand white-box + async fetch       */
	uint32_t miss_filled;   /* async fills committed to the ring       */
	uint32_t evictions;     /* ring LRU drops                          */
	uint32_t oversize;      /* glyphs dropped: bigger than a loader    */
				/* buffer (STG_BMP / SCRATCH_BYTES).       */
				/* Non-zero means holes in the page; the  */
				/* two buffers are bake-derived, so this  */
				/* should never grow except on a bake/card */
				/* mismatch.                              */
} aster_font_sd_stats_t;

void aster_font_sd_stats(aster_font_sd_stats_t *out);

#endif /* ASTER_FONT_SD_H */
