/* SPDX-License-Identifier: MIT */
#ifndef ASTER_AUDIO_FILE_READER_H
#define ASTER_AUDIO_FILE_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <zephyr/fs/fs.h>

/* Single file producer, owned exclusively by the player. Start after all
 * synchronous header/seek IO; stop before seeking, closing or freeing a file.
 * read() never waits for SD. Buffered data precedes EOF or an IO error. */
int audio_file_reader_start(struct fs_file_t *file, size_t remaining);
int audio_file_reader_read(void *dst, size_t size, bool *eof);
void audio_file_reader_stop(void);

#endif
