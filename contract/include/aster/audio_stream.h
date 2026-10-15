/* SPDX-License-Identifier: MIT. ABI 3.10: bounded compressed audio input. */
#ifndef ASTER_AUDIO_STREAM_H
#define ASTER_AUDIO_STREAM_H
#include "types.h"
#define ASTER_AUDIO_STREAM_MP3 1u
#define ASTER_AUDIO_STREAM_BUFFERING 1u
#define ASTER_AUDIO_STREAM_PLAYING 2u
#define ASTER_AUDIO_STREAM_PAUSED 3u
#define ASTER_AUDIO_STREAM_DONE 4u
#define ASTER_AUDIO_STREAM_FAILED 5u
#define ASTER_AUDIO_STREAM_CAPACITY 32768u
#define ASTER_AUDIO_STREAM_WRITE_MAX 2048u
#define ASTER_AUDIO_STREAM_F_EOF 1u
typedef struct {
    uint32_t size, version, state;
    int32_t error;
    uint32_t queued, capacity, underruns, flags;
    uint64_t received, consumed;
    uint32_t played_ms, reserved;
} aster_audio_stream_status_t; /* 56 bytes */
/* Requires audio-out and audio-stream capabilities, no net/fs permission.
 * Only MP3, one global audio session. Positive opaque session or stable error.
 * EBUSY when other playback/record is active. Caller retries on later events.
 * 32 KiB input ring + 16 KiB decoder window in PSRAM, not a whole-song copy.
 * Starts output after ~24 KiB or EOF; resumes buffering on starvation.
 */
ASTER_I("aster_audio_stream_open") int32_t aster_audio_stream_open(uint32_t codec);
/* Returns accepted bytes (may be partial), EAGAIN when full/busy, EPIPE after
 * finish, EBADF for foreign/closed handles. len 1..2048, copied before return.
 * Keep any unaccepted suffix. Never advance HTTP input by unaccepted bytes. */
ASTER_I("aster_audio_stream_write") int32_t aster_audio_stream_write(
    uint32_t handle, const void *data, uint32_t len);
/* Signals successful source EOF, then drains decoder; idempotent. Transport
 * failure should close, not finish. Finish alone does not prove playback DONE. */
ASTER_I("aster_audio_stream_finish") int32_t aster_audio_stream_finish(uint32_t handle);
ASTER_I("aster_audio_stream_status") int32_t aster_audio_stream_status(
    uint32_t handle, aster_audio_stream_status_t *status, uint32_t size);
/* Nonblocking close invalidates immediately, asynchronously discards buffers;
 * quota can remain briefly busy. APP exit/trap/init failure also closes.
 * Terminal status retains quota until close. Existing audio_pause/resume apply
 * to the stream, audio_stop cancels it. Seek is unsupported. */
ASTER_I("aster_audio_stream_close") int32_t aster_audio_stream_close(uint32_t handle);
#endif
