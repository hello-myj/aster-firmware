/* SPDX-License-Identifier: MIT */
#pragma once
#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "aster_idf_audio.h"

struct device {
    int role;
};
extern const struct device aster_idf_gpio_device;

struct gpio_dt_spec {
    const struct device *port;
    unsigned pin;
};

extern const struct gpio_dt_spec aster_idf_audio_pa;

#define GPIO_OUTPUT_INACTIVE 0
int gpio_is_ready_dt(const struct gpio_dt_spec *pin);
int gpio_pin_configure_dt(const struct gpio_dt_spec *pin, int mode);
int gpio_pin_set_dt(const struct gpio_dt_spec *pin, int value);
const struct device *aster_idf_audio_i2s(void);
bool device_is_ready(const struct device *device);

#ifdef ASTER_I2S_MEDIA_PCM
/* Reserve the shared codec and TX channel for the serialized media worker. */
int aster_i2s_media_acquire(uint32_t rate);
void aster_i2s_media_release(void);
#endif

/* Private Zephyr-shaped adapter for the existing board-independent player. */
enum i2s_dir { ASTER_I2S_TX = 1, ASTER_I2S_RX = 2, ASTER_I2S_BOTH = 3 };

#define I2S_TRIGGER_DROP 0
#define I2S_TRIGGER_START 1
#define I2S_TRIGGER_DRAIN 2
#define I2S_TRIGGER_PREPARE 3
#define I2S_FMT_DATA_FORMAT_I2S 0
#define I2S_OPT_FRAME_CLK_CONTROLLER 1
#define I2S_OPT_BIT_CLK_CONTROLLER 2

struct i2s_config {
    unsigned word_size, channels, format, options, frame_clk_freq;
    struct k_mem_slab *mem_slab;
    size_t block_size;
    unsigned timeout;
};

int i2s_configure(const struct device *device, enum i2s_dir dir, const struct i2s_config *config);
int i2s_trigger(const struct device *device, enum i2s_dir dir, int trigger);
int i2s_write(const struct device *device, void *block, size_t size);
int i2s_read(const struct device *device, void **block, size_t *size);
