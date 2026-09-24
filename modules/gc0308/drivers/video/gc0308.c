/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * GalaxyCore GC0308 sensor driver (QVGA RGB565).
 * Register defaults adapted from espressif/esp32-camera (Apache-2.0).
 */

#define DT_DRV_COMPAT galaxycore_gc0308

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "video_device.h"

LOG_MODULE_REGISTER(gc0308, CONFIG_VIDEO_LOG_LEVEL);

#define GC0308_PID_REG   0x00
#define GC0308_PID_VAL   0x9b
#define GC0308_PAGE      0xfe
#define GC0308_OUTPUT_FMT 0x24
#define GC0308_CISCTL_MODE1 0x14

#define VGA_W 640
#define VGA_H 480
#define QVGA_W 320
#define QVGA_H 240

struct gc0308_reg {
	uint8_t addr;
	uint8_t value;
};

struct gc0308_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec pwdn_gpio;
	struct gpio_dt_spec reset_gpio;
};

struct gc0308_data {
	struct video_format fmt;
};

static const struct gc0308_reg default_regs[] = {
#include "gc0308_settings.inc"
};

static const struct video_format_cap fmts[] = {
	{
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width_min = QVGA_W,
		.width_max = QVGA_W,
		.height_min = QVGA_H,
		.height_max = QVGA_H,
		.width_step = 0,
		.height_step = 0,
	},
	{ 0 },
};

static int gc0308_write(const struct i2c_dt_spec *i2c, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };

	return i2c_write_dt(i2c, buf, sizeof(buf));
}

static int gc0308_read(const struct i2c_dt_spec *i2c, uint8_t reg, uint8_t *val)
{
	return i2c_write_read_dt(i2c, &reg, 1, val, 1);
}

static int gc0308_write_all(const struct i2c_dt_spec *i2c, const struct gc0308_reg *regs,
			    size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int ret = gc0308_write(i2c, regs[i].addr, regs[i].value);

		if (ret) {
			return ret;
		}
	}

	return 0;
}

static int gc0308_set_bits(const struct i2c_dt_spec *i2c, uint8_t reg, uint8_t mask,
			   uint8_t value)
{
	uint8_t cur;
	int ret = gc0308_read(i2c, reg, &cur);

	if (ret) {
		return ret;
	}

	cur = (cur & ~mask) | (value & mask);
	return gc0308_write(i2c, reg, cur);
}

static int gc0308_set_qvga_window(const struct i2c_dt_spec *i2c)
{
	const uint16_t w = QVGA_W;
	const uint16_t h = QVGA_H;
	const uint16_t row_s = (VGA_H - h) / 2;
	const uint16_t col_s = (VGA_W - w) / 2;
	int ret;

	ret = gc0308_write(i2c, GC0308_PAGE, 0x00);
	if (ret) {
		return ret;
	}

	/* Windowing mode (esp32-camera WINDOWING_MODE) */
	ret |= gc0308_write(i2c, 0xf7, col_s / 4);
	ret |= gc0308_write(i2c, 0xf8, row_s / 4);
	ret |= gc0308_write(i2c, 0xf9, (col_s + w) / 4);
	ret |= gc0308_write(i2c, 0xfa, (row_s + h) / 4);

	ret |= gc0308_write(i2c, 0x05, row_s >> 8);
	ret |= gc0308_write(i2c, 0x06, row_s & 0xff);
	ret |= gc0308_write(i2c, 0x07, col_s >> 8);
	ret |= gc0308_write(i2c, 0x08, col_s & 0xff);

	ret |= gc0308_write(i2c, 0x09, (h + 8) >> 8);
	ret |= gc0308_write(i2c, 0x0a, (h + 8) & 0xff);
	ret |= gc0308_write(i2c, 0x0b, (w + 8) >> 8);
	ret |= gc0308_write(i2c, 0x0c, (w + 8) & 0xff);

	return ret;
}

static int gc0308_set_rgb565(const struct i2c_dt_spec *i2c)
{
	int ret = gc0308_write(i2c, GC0308_PAGE, 0x00);

	if (ret) {
		return ret;
	}

	/* OUTPUT_FMT[3:0] = 0x6 → RGB565 */
	return gc0308_set_bits(i2c, GC0308_OUTPUT_FMT, 0x0f, 0x06);
}

/*
 * PCLK divider (0x28[6:4], 1 = /2): halves the pixel output rate while the
 * sensor core keeps running at a stable XCLK. The board's PSRAM runs at
 * 40MHz (flash/PSRAM share the MSPI clock), so the DVP stream must stay
 * low-bandwidth; dividing PCLK (instead of dropping XCLK below the sensor
 * minimum) avoids occasional corrupt frames from an unstable pixel clock.
 */
#define GC0308_PCLK_DIV 0x28U

static int gc0308_set_pclk_div(const struct i2c_dt_spec *i2c)
{
	int ret = gc0308_write(i2c, GC0308_PAGE, 0x00);

	if (ret) {
		return ret;
	}
	return gc0308_set_bits(i2c, GC0308_PCLK_DIV, 0x70, 0x10);
}

static int gc0308_set_fmt(const struct device *dev, struct video_format *fmt)
{
	const struct gc0308_config *cfg = dev->config;
	struct gc0308_data *data = dev->data;
	int ret;

	if (fmt->pixelformat != VIDEO_PIX_FMT_RGB565 ||
	    fmt->width != QVGA_W || fmt->height != QVGA_H) {
		return -ENOTSUP;
	}

	ret = gc0308_set_rgb565(&cfg->i2c);
	if (ret) {
		return ret;
	}

	ret = gc0308_set_qvga_window(&cfg->i2c);
	if (ret) {
		return ret;
	}

	ret = gc0308_set_pclk_div(&cfg->i2c);
	if (ret) {
		return ret;
	}

	data->fmt = *fmt;
	data->fmt.pitch = fmt->width * 2;
	k_msleep(30);
	return 0;
}

static int gc0308_get_fmt(const struct device *dev, struct video_format *fmt)
{
	struct gc0308_data *data = dev->data;

	*fmt = data->fmt;
	return 0;
}

static int gc0308_get_caps(const struct device *dev, struct video_caps *caps)
{
	ARG_UNUSED(dev);
	caps->format_caps = fmts;
	return 0;
}

static int gc0308_set_stream(const struct device *dev, bool enable, enum video_buf_type type)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(enable);
	ARG_UNUSED(type);
	return 0;
}

static DEVICE_API(video, gc0308_driver_api) = {
	.set_format = gc0308_set_fmt,
	.get_format = gc0308_get_fmt,
	.get_caps = gc0308_get_caps,
	.set_stream = gc0308_set_stream,
};

static int gc0308_init(const struct device *dev)
{
	const struct gc0308_config *cfg = dev->config;
	struct gc0308_data *data = dev->data;
	struct video_format fmt = {
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width = QVGA_W,
		.height = QVGA_H,
	};
	uint8_t pid = 0;
	int ret;

	if (!device_is_ready(cfg->i2c.bus)) {
		return -ENODEV;
	}

	if (cfg->pwdn_gpio.port) {
		if (!gpio_is_ready_dt(&cfg->pwdn_gpio)) {
			return -ENODEV;
		}
		/* active-high PWDN → inactive = powered on */
		ret = gpio_pin_configure_dt(&cfg->pwdn_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret) {
			return ret;
		}
		k_msleep(10);
	}

	if (cfg->reset_gpio.port) {
		if (!gpio_is_ready_dt(&cfg->reset_gpio)) {
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret) {
			return ret;
		}
		k_msleep(5);
		gpio_pin_set_dt(&cfg->reset_gpio, 0);
		k_msleep(10);
	}

	ret = gc0308_write(&cfg->i2c, GC0308_PAGE, 0x00);
	if (ret) {
		LOG_ERR("GC0308 I2C write failed: %d", ret);
		return ret;
	}

	ret = gc0308_read(&cfg->i2c, GC0308_PID_REG, &pid);
	if (ret || pid != GC0308_PID_VAL) {
		LOG_ERR("GC0308 not found (pid=0x%02x ret=%d)", pid, ret);
		return -ENODEV;
	}
	LOG_INF("GC0308 detected (PID=0x%02x)", pid);

	/* Software reset */
	ret = gc0308_write(&cfg->i2c, GC0308_PAGE, 0xf0);
	if (ret) {
		return ret;
	}
	k_msleep(80);

	ret = gc0308_write_all(&cfg->i2c, default_regs, ARRAY_SIZE(default_regs));
	if (ret) {
		LOG_ERR("GC0308 default regs failed: %d", ret);
		return ret;
	}
	k_msleep(80);

	ret = gc0308_set_fmt(dev, &fmt);
	if (ret) {
		LOG_ERR("GC0308 set_fmt failed: %d", ret);
		return ret;
	}

	data->fmt = fmt;
	data->fmt.pitch = QVGA_W * 2;
	return 0;
}

#define GC0308_INIT(n)                                                                             \
	static const struct gc0308_config gc0308_cfg_##n = {                                       \
		.i2c = I2C_DT_SPEC_INST_GET(n),                                                    \
		.pwdn_gpio = GPIO_DT_SPEC_INST_GET_OR(n, pwdn_gpios, { 0 }),                       \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(n, reset_gpios, { 0 }),                     \
	};                                                                                         \
	static struct gc0308_data gc0308_data_##n;                                                 \
	DEVICE_DT_INST_DEFINE(n, gc0308_init, NULL, &gc0308_data_##n, &gc0308_cfg_##n,             \
			      POST_KERNEL, CONFIG_VIDEO_INIT_PRIORITY, &gc0308_driver_api);        \
	VIDEO_DEVICE_DEFINE(gc0308_##n, DEVICE_DT_INST_GET(n), NULL);

DT_INST_FOREACH_STATUS_OKAY(GC0308_INIT)
