/* SPDX-License-Identifier: MIT
 * Load UTF-8 lyrics for a play session: ID3 USLT/SYLT, else sibling .lrc.
 */
#ifndef ASTER_AUDIO_LYRICS_H
#define ASTER_AUDIO_LYRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void aster_audio_lyrics_clear(void);
void aster_audio_lyrics_load_path(const char *path, bool mp3);
int  aster_audio_lyrics_copy(char *dst, size_t max);

#endif
