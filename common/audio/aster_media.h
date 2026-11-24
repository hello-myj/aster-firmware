/* SPDX-License-Identifier: MIT */
#ifndef ASTER_MEDIA_H
#define ASTER_MEDIA_H
#include <stdint.h>
#include <stddef.h>
#define ASTER_MEDIA_OUTPUT_SYSTEM 0u
#define ASTER_MEDIA_IDLE 0u
#define ASTER_MEDIA_OPENING 1u
#define ASTER_MEDIA_PLAYING 2u
#define ASTER_MEDIA_PAUSED 3u
#define ASTER_MEDIA_ENDED 4u
#define ASTER_MEDIA_ERROR 5u
#define ASTER_MEDIA_STOPPING 6u
#define ASTER_MEDIA_PATH_MAX 256u
typedef struct {
 uint32_t size,version,state,rate,channels,bits,codec,endpoint;
 int32_t error;
 uint64_t frame,total_frames;
 size_t decoder_bytes,decoder_peak;
 char path[ASTER_MEDIA_PATH_MAX];
} aster_media_snapshot_t;
/* Internal system-owned session: survives a VM/UI exit. Boot init is optional
 * and creates no task. Start/control never perform file I/O on the caller.
 * Native negative errno, not public APP ABI. */
int aster_media_init(void);
/* Endpoint zero follows the system output. A newly available output takes
 * over at a PCM block boundary. Explicitly addressed sessions stay pinned. */
int aster_media_play(const char *path,uint32_t endpoint);
void aster_media_set_default_output(uint32_t endpoint);
uint32_t aster_media_default_output(void);
int aster_media_pause(void);
int aster_media_resume(void);
int aster_media_seek(uint32_t milliseconds);
int aster_media_stop(void);
int aster_media_stop_output(uint32_t endpoint);
int aster_media_snapshot(aster_media_snapshot_t *out);
#endif
