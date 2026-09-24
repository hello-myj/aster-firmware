/* SPDX-License-Identifier: MIT
 * ASTER ABI audio (1.7 transport + 1.8 MP3 + 1.9 streaming + 1.13 seek/lyrics).
 *
 * Gated by ASTER_FEATURE_AUDIO_OUTPUT / ASTER_FEATURE_AUDIO_INPUT only — audio
 * is a hardware fact, not a permission surface (same rule as camera/sensor).
 *
 * 2.0 cleanup: aster_audio_play / aster_audio_start were removed. Playback goes
 * through the background session (start_file streams a sandboxed path —
 * the old in-memory copy route could not exceed 2 MiB) or the blocking
 * aster_audio_record capture. MP3 is decoded in firmware; playback sessions
 * are start_file only.
 *
 * 3.2: aster_audio_get_info is back. 2.0 dropped it because the native format
 * was "fixed at compile time" — but fixed *per board*, and this header is
 * compiled into an app once and installed on any board. So ASTER_AUDIO_RATE
 * &co. were a number that is right on exactly one board and silently wrong
 * on the others (a wrong rate does not fail; it records and plays at the
 * wrong speed). The macros are deleted and the format is asked for.
 *
 * 1.13: aster_audio_seek(msec) jumps a background session; get_state v2 adds
 * msec_done/msec_total. aster_audio_get_lyrics copies UTF-8 lyrics (ID3 USLT/
 * SYLT, else a sibling .lrc).
 */
#ifndef ASTER_AUDIO_H
#define ASTER_AUDIO_H

#include "types.h"

#define ASTER_AUDIO_IDLE    0u
#define ASTER_AUDIO_PLAYING 1u
#define ASTER_AUDIO_PAUSED  2u

/* Native format descriptor (asked for with aster_audio_get_info). The reference
 * port answers 16000/2/16 -- little-endian signed 16-bit, interleaved stereo
 * -- but read it from here rather than assuming: another board's codec may
 * run at 48 kHz, and its samples are the ones your buffer will meet. */
typedef struct {
	uint32_t size;        /* sizeof(aster_audio_info_t) */
	uint32_t version;     /* 1 */
	uint32_t sample_rate; /* Hz */
	uint32_t channels;
	uint32_t bits;
	uint32_t flags;       /* reserved, V1 = 0 */
} aster_audio_info_t;

typedef struct {
	uint32_t size;        /* sizeof(aster_audio_state_t) */
	uint32_t version;     /* 1: bytes; 2: + msec_done/msec_total */
	uint32_t state;       /* ASTER_AUDIO_IDLE / PLAYING / PAUSED */
	uint32_t bytes_total;
	uint32_t bytes_done;
	uint32_t volume;      /* 0..100 */
	uint32_t msec_done;   /* [1.13] elapsed, native PCM clock */
	uint32_t msec_total;  /* [1.13] duration estimate, 0 if unknown */
} aster_audio_state_t;

/* ---- 3.2 ---- */

/* Fill `info` with this board's native PCM format (`size` is
 * sizeof(aster_audio_info_t); a shorter size is accepted and filled as far as
 * it goes). Returns ASTER_OK, ASTER_EINVAL, or ASTER_ENODEV (no audio out). Use it
 * to size buffers and to convert byte counts to milliseconds; do not assume
 * 16 kHz stereo. */
ASTER_I("aster_audio_get_info") int32_t aster_audio_get_info(aster_audio_info_t *info, uint32_t size);

/* ---- 1.7 ---- */

/* DAC volume 0..100 (0 = mute, 100 = unity / 0 dB). Percent is loudness
 * (even dB steps), not a linear amplitude scale. Applies immediately. */
ASTER_I("aster_audio_set_volume") int32_t aster_audio_set_volume(int32_t percent);
/* Returns 0..100, or a negative aster_error_t. */
ASTER_I("aster_audio_get_volume") int32_t aster_audio_get_volume(void);

/* Record into `pcm` until `size` bytes are filled (blocking; same PCM
 * contract as playback). Returns ASTER_OK, ASTER_ENODEV (no input), ASTER_EINVAL,
 * ASTER_EBUSY, or ASTER_EIO. */
ASTER_I("aster_audio_record") int32_t aster_audio_record(uint8_t *pcm, uint32_t size);

/* Stream a sandboxed path in the background (WAV/PCM/MP3). Replaces any
 * current session. Typical library root is "/music/…" → /SD:/music.
 * Needs AUDIO_OUTPUT plus the `fs` permission. Returns ASTER_OK, ASTER_ENODEV,
 * ASTER_EPERM, ASTER_EINVAL, ASTER_ENOENT, or ASTER_EIO. */
ASTER_I("aster_audio_start_file") int32_t aster_audio_start_file(const char *path, uint32_t path_len);

ASTER_I("aster_audio_pause") int32_t aster_audio_pause(void);   /* ESTATE if idle */
ASTER_I("aster_audio_resume") int32_t aster_audio_resume(void); /* ESTATE if idle */
ASTER_I("aster_audio_stop") int32_t aster_audio_stop(void);     /* OK if already idle */

ASTER_I("aster_audio_get_state") int32_t aster_audio_get_state(aster_audio_state_t *st, uint32_t size);

/* ---- 1.13 ---- */

/* Jump the background session to `msec` from the start (clamped). WAV/PCM
 * is sample-accurate; MP3 is a byte-proportional estimate. ASTER_ESTATE if
 * idle. */
ASTER_I("aster_audio_seek") int32_t aster_audio_seek(uint32_t msec);

/* Copy UTF-8 lyrics for the current (or last started) session.
 * Returns bytes copied (0 = none), or a negative ASTER_*. */
ASTER_I("aster_audio_get_lyrics") int32_t aster_audio_get_lyrics(char *buf, uint32_t size);

#endif /* ASTER_AUDIO_H */
