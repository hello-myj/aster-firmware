/* SPDX-License-Identifier: MIT
 * GC0308 via ESP32-S3 LCD_CAM DVP -> single-frame RGB565 capture to SD.
 * Modeled on lckfb_esp32s3/src/features/cam.c (no LVGL, no preview).
 */
#include "aster_platform.h"
#include "aster_storage.h"

#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/video.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aster_cam, LOG_LEVEL_INF);

/* Capture geometry is this board's (Kconfig), not a hardware-neutral
 * constant: the runtime used to size-check app buffers against a copy of
 * these numbers baked into the shared port contract header. */
#define CAM_W CONFIG_ASTER_CAM_WIDTH
#define CAM_H CONFIG_ASTER_CAM_HEIGHT
#define CAM_BYTES (CAM_W * CAM_H * 2)
#define CAM_NBUF 3

/* Named, not indexed -- see the zephyr,user block in the board overlay for why
 * a positional array of control pins is a silent-wrong-pin bug waiting for the
 * first board that writes its pins down in a different order. */
static const struct gpio_dt_spec cam_pwdn =
	GPIO_DT_SPEC_GET_OR(DT_PATH(zephyr_user), cam_pwdn_gpios, {0});

/* Shared capture core: DVP one-shot, hand the RGB565 frame to sink().
 * sink NULL = probe-only (power up, stream, discard, power down). The sink
 * receives the driver's own DMA buffer -- copying/writing happens there so
 * the core never allocates a 150 KB bounce buffer (main kheap is only 96 KB).
 */
static int cam_capture_core(int (*sink)(const uint8_t *rgb565, size_t len, void *u),
			    void *ud)
{
	const struct device *cam_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_camera));
	if (!cam_dev || !device_is_ready(cam_dev)) {
		return -ENODEV;
	}

	/* power up sensor */
	if (gpio_is_ready_dt(&cam_pwdn)) {
		(void)gpio_pin_configure_dt(&cam_pwdn, GPIO_OUTPUT_INACTIVE);
		k_msleep(20);
	}

	struct video_format fmt = {
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width = CAM_W,
		.height = CAM_H,
		.type = VIDEO_BUF_TYPE_OUTPUT,
	};
	int err = video_set_format(cam_dev, &fmt);
	if (err) {
		LOG_ERR("set_format %d", err);
		goto out;
	}
	video_get_format(cam_dev, &fmt);

	struct video_buffer *vbufs[CAM_NBUF] = {0};
	size_t bsz = fmt.size ? fmt.size : CAM_BYTES;
	for (int i = 0; i < CAM_NBUF; i++) {
		vbufs[i] = video_buffer_aligned_alloc(bsz, CONFIG_VIDEO_BUFFER_POOL_ALIGN,
						      K_NO_WAIT);
		if (!vbufs[i]) {
			/* The pool is shared_multi_heap (SMH_REG_ATTR_EXTERNAL),
			 * i.e. the same PSRAM every other subsystem draws from.
			 * Say which allocation died: "no buffer at all" and "the
			 * third one" are very different budgets. */
			LOG_ERR("vbuf %d/%d (%u B) alloc failed", i, CAM_NBUF,
				(unsigned)bsz);
			err = -ENOMEM;
			goto release;
		}
		vbufs[i]->type = VIDEO_BUF_TYPE_OUTPUT;
		err = video_enqueue(cam_dev, vbufs[i]);
		if (err) {
			LOG_ERR("enqueue %d -> %d", i, err);
			goto release;
		}
	}
	err = video_stream_start(cam_dev, VIDEO_BUF_TYPE_OUTPUT);
	if (err) {
		LOG_ERR("stream_start %d", err);
		goto release;
	}

	/* Discard a few unstable frames, keeping only one borrowed at a
	 * time. Older frames go BACK into the driver queue: esp32-dvp pulls
	 * from fifo_in and never reclaims released buffers, so releasing
	 * instead of re-enqueueing drains the pool until "Frame dropped. No
	 * buffer available" and dequeue times out with -EAGAIN.
	 */
	struct video_buffer *got = NULL;
	for (int warm = 0; warm < 5; warm++) {
		struct video_buffer *vb = NULL;
		err = video_dequeue(cam_dev, &vb, K_MSEC(500));
		if (err) {
			(void)video_stream_stop(cam_dev, VIDEO_BUF_TYPE_OUTPUT);
			goto release;
		}
		if (got && video_enqueue(cam_dev, got) < 0) {
			video_buffer_release(got);
		}
		got = vb;
	}
	(void)video_stream_stop(cam_dev, VIDEO_BUF_TYPE_OUTPUT);

	if (!got) {
		err = -EIO;
		goto release;
	}
	sys_cache_data_invd_range(got->buffer, bsz);
	if (sink) {
		err = sink(got->buffer, CAM_BYTES, ud);
	}
	/* got is one of vbufs[]; the release loop below frees it once */

release:
	for (int i = 0; i < CAM_NBUF; i++) {
		struct video_buffer *vb = NULL;
		while (video_dequeue(cam_dev, &vb, K_NO_WAIT) == 0) {
			/* drain leftovers (driver frees them) */
		}
		if (vbufs[i]) {
			video_buffer_release(vbufs[i]);
		}
	}
out:
	if (gpio_is_ready_dt(&cam_pwdn)) {
		(void)gpio_pin_configure_dt(&cam_pwdn, GPIO_OUTPUT_INACTIVE);
	}
	return err;
}

static int sink_memcpy(const uint8_t *frame, size_t len, void *u)
{
	memcpy(u, frame, len);
	return 0;
}

int aster_plat_cam_grab_rgb565(uint8_t *dst, size_t dst_len)
{
	if (dst_len < CAM_BYTES) {
		return -EINVAL;
	}
	return cam_capture_core(sink_memcpy, dst);
}

int aster_plat_cam_info(uint32_t *w, uint32_t *h)
{
	if (w) {
		*w = CAM_W;
	}
	if (h) {
		*h = CAM_H;
	}
	return 0;
}

static int sink_file(const uint8_t *frame, size_t len, void *u)
{
	aster_fs_write_safe((struct fs_file_t *)u, frame, len);
	return 0;
}

int aster_plat_cam_capture_rgb565(const char *path)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	int err = fs_open(&f, path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
	if (err) {
		return err;
	}
	err = cam_capture_core(sink_file, &f);
	fs_close(&f);
	if (err) {
		(void)fs_unlink(path); /* drop a half-written file on failure */
		return err;
	}
	LOG_INF("cam: wrote %s (%u bytes)", path, (unsigned)CAM_BYTES);
	return 0;
}
