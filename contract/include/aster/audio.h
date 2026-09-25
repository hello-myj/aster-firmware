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

/* DAC volume 0..100 (0 = mute). The reference ES8311 port reaches +6 dB at
 * 100; loud sources may distort at the high end. This is a loudness control,
 * not a linear amplitude scale. Applies immediately. */
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

/* ABI 3.4: preload up to four short native-format stereo PCM WAV effects
 * (max 64 KiB each) from a sandboxed path before starting music. Returns a
 * positive handle or ASTER_* error. play mixes one effect into the current
 * background stream without replacing it, or plays over silence when idle.
 * Different handles play together; replaying one handle restarts that cue.
 * Unload releases its PSRAM copy.
 * load requires the fs permission. */
ASTER_I("aster_audio_effect_load") int32_t aster_audio_effect_load(const char *path, uint32_t path_len);
ASTER_I("aster_audio_effect_play") int32_t aster_audio_effect_play(uint32_t handle);
ASTER_I("aster_audio_effect_unload") int32_t aster_audio_effect_unload(uint32_t handle);

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
