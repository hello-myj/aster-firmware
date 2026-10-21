/* SPDX-License-Identifier: MIT
 * FT6336 two-point I2C poller. Reads one complete register frame and hands
 * it to the platform input state machine atomically.
 */
#define DT_DRV_COMPAT focaltech_ft6336_mt

#include "aster_platform.h"
#include "touch_protocol.h"

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aster_ft6336, LOG_LEVEL_INF);

struct ft6336_config {
	struct i2c_dt_spec bus;
	uint16_t width;
	uint16_t height;
	uint16_t poll_ms;
};

struct ft6336_data {
	const struct device *dev;
	struct k_work_delayable work;
	uint8_t invalid_frames;
};

static void ft6336_poll(struct k_work *work)
{
	struct ft6336_data *data = CONTAINER_OF(k_work_delayable_from_work(work),
							struct ft6336_data, work);
	const struct ft6336_config *config = data->dev->config;
	/* TD_STATUS through P2_MISC, read in a single I2C transaction so the
	 * count, IDs, events, and both coordinates belong to one sample. */
	uint8_t report[ASTER_FT6336_REPORT_BYTES];
	int err = i2c_burst_read_dt(&config->bus, 0x02, report, sizeof(report));
	if (err) {
		aster_plat_input_touch_report_error((uint64_t)k_uptime_get());
		goto again;
	}
	struct aster_touch_frame frame;
	err = aster_ft6336_decode(report, sizeof(report), config->width,
				  config->height, &frame);
	if (err) {
		if (++data->invalid_frames == 50) {
			LOG_WRN("repeated invalid FT6336 frames (%d)", err);
			data->invalid_frames = 0;
		}
		aster_plat_input_touch_report_error((uint64_t)k_uptime_get());
	} else {
		data->invalid_frames = 0;
		aster_plat_input_touch_report_frame(&frame);
	}
again:
	k_work_reschedule(&data->work, K_MSEC(config->poll_ms));
}

static int ft6336_init(const struct device *dev)
{
	struct ft6336_data *data = dev->data;
	const struct ft6336_config *config = dev->config;
	if (!i2c_is_ready_dt(&config->bus)) {
		aster_plat_input_touch_set_ready(false);
		return -ENODEV;
	}
	data->dev = dev;
	k_work_init_delayable(&data->work, ft6336_poll);
	aster_plat_input_touch_set_ready(true);
	k_work_schedule(&data->work, K_MSEC(config->poll_ms));
	LOG_INF("FT6336 two-point input ready (%ux%u, %u ms)",
		config->width, config->height, config->poll_ms);
	return 0;
}

#define FT6336_DEFINE(inst) \
	static const struct ft6336_config ft6336_config_##inst = { \
		.bus = I2C_DT_SPEC_INST_GET(inst), \
		.width = DT_INST_PROP(inst, screen_width), \
		.height = DT_INST_PROP(inst, screen_height), \
		.poll_ms = DT_INST_PROP(inst, poll_interval_ms), \
	}; \
	static struct ft6336_data ft6336_data_##inst; \
	DEVICE_DT_INST_DEFINE(inst, ft6336_init, NULL, &ft6336_data_##inst, \
			      &ft6336_config_##inst, POST_KERNEL, \
			      CONFIG_INPUT_INIT_PRIORITY, NULL);
DT_INST_FOREACH_STATUS_OKAY(FT6336_DEFINE)
