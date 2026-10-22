/* SPDX-License-Identifier: MIT */
#include "audio_file_reader.h"
#include "audio_stream_ring.h"
#include "aster_mem.h"
#include "aster_storage.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define FILE_READ_CAPACITY (16u * 1024u)
#define FILE_READ_CHUNK 4096u
#define FILE_READ_STACK 3072u

static K_MUTEX_DEFINE(reader_mu);
static K_SEM_DEFINE(reader_space, 0, 1);
static struct k_thread reader_thread;
static struct audio_stream_ring reader_ring;
static struct fs_file_t *reader_file;
static void *reader_stack;
static size_t reader_remaining;
static bool reader_running, reader_stop, reader_eof;
static int reader_error;

static void reader_main(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		k_mutex_lock(&reader_mu, K_FOREVER);
		if (reader_stop || !reader_remaining) {
			reader_eof = !reader_remaining;
			k_mutex_unlock(&reader_mu);
			return;
		}
		size_t tail = (reader_ring.head + reader_ring.used) % reader_ring.cap;
		size_t n = MIN(reader_ring.cap - tail,
			       MIN(FILE_READ_CHUNK, reader_remaining));
		/* Batch SD transactions rather than reading every small amount
		 * released by a single decoded MP3 frame. */
		if (reader_ring.cap - reader_ring.used < n) {
			n = 0;
		}
		k_mutex_unlock(&reader_mu);
		if (!n) {
			k_sem_take(&reader_space, K_FOREVER);
			continue;
		}
		/* Only this producer writes the free span; the consumer can only
		 * release more space. Publish bytes after IO, never hold the mutex
		 * while accessing SD, and never touch session/output locks here. */
		int got = aster_fs_read_safe(reader_file, reader_ring.data + tail, n);
		k_mutex_lock(&reader_mu, K_FOREVER);
		if (reader_stop) {
			k_mutex_unlock(&reader_mu);
			return;
		}
		if (got <= 0) {
			/* Size was established at open: an early zero is truncation,
			 * not a successful end of the promised payload. */
			reader_error = got < 0 ? got : -EIO;
			k_mutex_unlock(&reader_mu);
			return;
		}
		reader_ring.used += (size_t)got;
		reader_remaining -= (size_t)got;
		k_mutex_unlock(&reader_mu);
	}
}

void audio_file_reader_stop(void)
{
	/* Lifecycle operations are serialized by the player, not by the IO
	 * worker. In particular, never free its stack or buffer before join. */
	if (!reader_running) {
		return;
	}
	k_mutex_lock(&reader_mu, K_FOREVER);
	reader_stop = true;
	k_mutex_unlock(&reader_mu);
	k_sem_give(&reader_space);
	k_thread_join(&reader_thread, K_FOREVER);
	aster_psram_free(reader_ring.data);
	aster_dram_free(reader_stack);
	reader_ring = (struct audio_stream_ring){0};
	reader_stack = NULL;
	reader_file = NULL;
	reader_running = false;
}

int audio_file_reader_start(struct fs_file_t *file, size_t remaining)
{
	if (!file || reader_running) {
		return file ? -EBUSY : -EINVAL;
	}
	uint8_t *data = aster_psram_alloc(FILE_READ_CAPACITY);
	void *stack = aster_dram_alloc(K_KERNEL_STACK_LEN(FILE_READ_STACK));
	if (!data || !stack) {
		aster_psram_free(data);
		aster_dram_free(stack);
		return -ENOMEM;
	}
	reader_ring = (struct audio_stream_ring){.data = data, .cap = FILE_READ_CAPACITY};
	reader_stack = stack;
	reader_file = file;
	reader_remaining = remaining;
	reader_stop = reader_eof = false;
	reader_error = 0;
	k_sem_reset(&reader_space);
	reader_running = true;
	k_thread_create(&reader_thread, stack,
		K_KERNEL_STACK_LEN(FILE_READ_STACK) - K_KERNEL_STACK_RESERVED,
		reader_main, NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_FOREVER);
	k_thread_name_set(&reader_thread, "aster_aread");
	k_thread_start(&reader_thread);
	return 0;
}

int audio_file_reader_read(void *dst, size_t size, bool *eof)
{
	k_mutex_lock(&reader_mu, K_FOREVER);
	if (!reader_running) {
		k_mutex_unlock(&reader_mu);
		return -EPIPE;
	}
	size_t n = audio_ring_read(&reader_ring, dst, size);
	*eof = reader_eof && !reader_ring.used;
	int err = reader_ring.used ? 0 : reader_error;
	k_mutex_unlock(&reader_mu);
	if (n) {
		k_sem_give(&reader_space);
	}
	return n ? (int)n : err;
}
