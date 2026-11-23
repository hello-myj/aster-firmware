/* SPDX-License-Identifier: MIT. Continuous input for the shared I2S backend. */
static K_MUTEX_DEFINE(capture_mu);
static struct {
	struct audio_stream_ring ring;
	struct k_thread worker;
	void *stack;
	uint32_t handle;
	bool stopping, done, closing;
	int error;
} capture;
static uint32_t capture_generation;

static void capture_loop(void *a, void *b, void *c)
{
	(void)a;
	(void)b;
	(void)c;
	int err = 0;
	for (;;) {
		k_mutex_lock(&capture_mu, K_FOREVER);
		bool stopping = capture.stopping;
		k_mutex_unlock(&capture_mu);
		if (stopping)
			break;
		void *blk = NULL;
		size_t got = 0;
		k_mutex_lock(&audio_lock, K_FOREVER);
		err = i2s_read(i2s_dev, &blk, &got);
		if (!err && (!blk || !got || got > AUDIO_BLOCK || got % ASTER_PLAT_AUDIO_FRAME))
			err = -EIO;
		if (!err) {
			k_mutex_lock(&capture_mu, K_FOREVER);
			if (capture.ring.cap - capture.ring.used < got)
				err = -ENOSPC;
			else
				audio_ring_write(&capture.ring, blk, got);
			k_mutex_unlock(&capture_mu);
		}
		if (blk)
			k_mem_slab_free(&rx_slab, blk);
		if (!err)
			err = tx_silence();
		k_mutex_unlock(&audio_lock);
		if (err)
			break;
	}
	k_mutex_lock(&audio_lock, K_FOREVER);
	i2s_shutdown();
	k_mutex_unlock(&audio_lock);
	k_mutex_lock(&capture_mu, K_FOREVER);
	capture.error = err;
	capture.done = true;
	k_mutex_unlock(&capture_mu);
}

int aster_plat_audio_capture_open(aster_audio_info_t *info)
{
	if (!info)
		return -EINVAL;
	k_mutex_lock(&sess_mu, K_FOREVER);
	if (capture_owned || media_owned || input_stream.handle || sess.state != ASTER_AUDIO_IDLE) {
		k_mutex_unlock(&sess_mu);
		return -EBUSY;
	}
	capture_owned = true;
	k_mutex_unlock(&sess_mu);
	/* Half a second of native PCM absorbs storage and scheduling jitter. */
	size_t cap = (CONFIG_ASTER_AUDIO_RATE * ASTER_PLAT_AUDIO_FRAME + 1) / 2;
	cap = ((cap + AUDIO_BLOCK - 1) / AUDIO_BLOCK) * AUDIO_BLOCK;
	capture.ring = (struct audio_stream_ring){.data = aster_psram_alloc(cap), .cap = cap};
	capture.stack = aster_dram_alloc(K_KERNEL_STACK_LEN(4096));
	int err = -ENOMEM;
	if (capture.ring.data && capture.stack) {
		k_mutex_lock(&audio_lock, K_FOREVER);
		err = codecs_init();
		if (!err)
			err = record_begin();
		k_mutex_unlock(&audio_lock);
	}
	if (err) {
		aster_psram_free(capture.ring.data);
		aster_dram_free(capture.stack);
		capture_owned = false;
		return err;
	}
	k_mutex_lock(&capture_mu, K_FOREVER);
	if (++capture_generation > INT32_MAX)
		capture_generation = 1;
	capture.handle = capture_generation;
	capture.done = capture.stopping = capture.closing = false;
	capture.error = 0;
	*info = (aster_audio_info_t){.size = sizeof(*info), .version = 1,
	                            .sample_rate = CONFIG_ASTER_AUDIO_RATE,
	                            .channels = CONFIG_ASTER_AUDIO_CHANNELS,
	                            .bits = CONFIG_ASTER_AUDIO_BITS};
	k_thread_create(&capture.worker, capture.stack,
	                K_KERNEL_STACK_LEN(4096) - K_KERNEL_STACK_RESERVED, capture_loop,
	                NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
	int handle = (int)capture.handle;
	k_mutex_unlock(&capture_mu);
	return handle;
}

int aster_plat_audio_capture_read(uint32_t handle, uint8_t *pcm, size_t size)
{
	if (!pcm || !size || size % ASTER_PLAT_AUDIO_FRAME)
		return -EINVAL;
	k_mutex_lock(&capture_mu, K_FOREVER);
	int rc = -EBADF;
	if (handle && handle == capture.handle && !capture.closing) {
		rc = (int)audio_ring_read(&capture.ring, pcm, size);
		if (!rc)
			rc = capture.done ? capture.error : -EAGAIN;
	}
	k_mutex_unlock(&capture_mu);
	return rc;
}

int aster_plat_audio_capture_finish(uint32_t handle)
{
	k_mutex_lock(&capture_mu, K_FOREVER);
	int rc = !handle || handle != capture.handle || capture.closing ? -EBADF : 0;
	if (!rc)
		capture.stopping = true;
	k_mutex_unlock(&capture_mu);
	return rc;
}

int aster_plat_audio_capture_close(uint32_t handle)
{
	k_mutex_lock(&capture_mu, K_FOREVER);
	if (!handle || handle != capture.handle || capture.closing) {
		k_mutex_unlock(&capture_mu);
		return -EBADF;
	}
	capture.stopping = capture.closing = true;
	k_mutex_unlock(&capture_mu);
	k_thread_join(&capture.worker, K_FOREVER);
	k_mutex_lock(&capture_mu, K_FOREVER);
	aster_dram_free(capture.stack);
	aster_psram_free(capture.ring.data);
	capture.handle = 0;
	capture_owned = false;
	k_mutex_unlock(&capture_mu);
	return 0;
}
