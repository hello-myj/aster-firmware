#include "audio_format.h"
/* SPDX-License-Identifier: MIT
 * ES8311 DAC + ES7210 ADC over I2S0 (LCKFB ESP32-S3).
 * PCM play/record is 16 kHz / 16-bit / stereo s16le. MP3 is decoded here
 * to that native PCM before I2S (same play/start entry points).
 */
#include "aster_platform.h"
#include "aster_bytes.h"
#include "aster_mem.h"
#include "aster_storage.h"
#include "audio_lyrics.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <aster/audio.h>
#include <aster/audio_stream.h>
#include "audio_stream_ring.h"
#include "audio_rate.h"
#include <minimp3.h>

LOG_MODULE_REGISTER(aster_audio, LOG_LEVEL_INF);

#define DAC_ADDR 0x18 /* ES8311 */
#define ADC_ADDR 0x41 /* ES7210 (AD0 high); 0x40 tried as fallback */

#define AUDIO_BLOCK     512
#define AUDIO_TX_MAX    1536
#define AUDIO_NBLOCKS   6
#define AUDIO_TIMEOUT   2000
#define AUDIO_VOL_DEF   70
#define AUDIO_STACK     8192
#define MP3_FIFO_SAMP   2048
#define STREAM_WIN      16384
#define MP3_NEED        2304
#define EFFECT_SLOTS    4
#define EFFECT_MAX      (64u * 1024u)
#define EFFECT_IDLE_BLOCKS ((2u * CONFIG_ASTER_AUDIO_RATE * ASTER_PLAT_AUDIO_FRAME) / AUDIO_BLOCK)

/* Named, not indexed: this used to be index 1 of a shared io-gpios array, so a
 * board that listed its control pins in a different order would drive the
 * camera's power-down line from here -- silently, while looking correct at
 * both ends. See the zephyr,user block in the board overlay. */
static const struct gpio_dt_spec pa_en =
	GPIO_DT_SPEC_GET_OR(DT_PATH(zephyr_user), pa_en_gpios, {0});

/* The buses are bound by ROLE, not by node label (see the chosen block in the
 * board overlay). Without this the mismatch is invisible: DT_CHOSEN() on a
 * missing role yields an invalid node, DEVICE_DT_GET_OR_NULL() yields NULL,
 * and bring-up returns -ENODEV -- which on the bench looks exactly like a
 * codec that failed to probe. */
#if IS_ENABLED(CONFIG_ASTER_AUDIO)
BUILD_ASSERT(DT_HAS_CHOSEN(aster_audio_i2c) && DT_HAS_CHOSEN(aster_audio_i2s),
	     "the board conf claims an audio codec, but the board overlay has no "
	     "`chosen { aster,audio-i2c = &<bus>; aster,audio-i2s = &<i2s>; }` -- "
	     "codec bring-up would return -ENODEV forever");
#endif

static const struct device *i2c_dev;
static const struct device *i2s_dev;
static uint8_t adc_addr = ADC_ADDR;
static bool codecs_ready;
static bool pa_on;
static enum i2s_dir active_dir;
static uint8_t i2s_queued;
static uint8_t g_vol = AUDIO_VOL_DEF;
/* Protected by audio_lock; raw PCM/record stay at the ABI native format. */
static uint32_t output_rate = CONFIG_ASTER_AUDIO_RATE;
static size_t output_block = AUDIO_BLOCK;
static K_MUTEX_DEFINE(audio_lock);
static K_MUTEX_DEFINE(sess_mu);
static K_MUTEX_DEFINE(effect_mu);
static K_SEM_DEFINE(play_go, 0, 1);
static K_SEM_DEFINE(play_wake, 0, 1);
/* The player thread and its stack exist only while a session is running. */
static struct k_thread play_thread;
static void *play_stack;
static bool play_alive;
static bool play_join;
static K_MUTEX_DEFINE(play_life);

static struct fs_file_t sess_file;
static struct {
	uint8_t *pcm;
	size_t size;
	size_t off;
	uint32_t fraction;
	bool playing;
} effects[EFFECT_SLOTS];

/* Firmware UI voice: flash-resident mono PCM, independent of APP handles. */
static struct {
 const int16_t *pcm;
 size_t frames, off;
 unsigned gain, priority;
 uint32_t fraction;
} ui_voice;

static struct {
	uint8_t *buf;
	size_t size;
	size_t off;
	size_t win_len;
	size_t win_off;
	size_t file_base;
	uint32_t pcm_done;
	uint32_t pcm_rate;
	uint32_t msec_total;
	uint32_t seek_msec;
	uint32_t state;
	bool pause;
	bool stop;
	bool mp3;
	bool from_file;
	bool from_stream;
	bool effect_only;
	bool effect_closing;
	bool file_open;
	bool seek_pending;
	int err;
} sess;

static struct {
    struct audio_stream_ring ring;
    uint32_t handle, state, underruns, played_ms;
    uint64_t received, consumed;
    int error;
    bool eof, buffering, id3_checked;
    size_t id3_left;
} input_stream;
static uint32_t input_generation;

static struct {
	mp3dec_t dec;
	int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
	int16_t fifo[MP3_FIFO_SAMP];
	unsigned fifo_n;
	int hz;
	int ch;
	unsigned wrote;
} mp3p __attribute__((section(".ext_ram.bss.mp3p"), aligned(16)));

K_MEM_SLAB_DEFINE_STATIC(tx_slab, AUDIO_TX_MAX, AUDIO_NBLOCKS, 32);
K_MEM_SLAB_DEFINE_STATIC(rx_slab, AUDIO_BLOCK, AUDIO_NBLOCKS, 32);

/* Half-scale 64-sample sine (one period). */
static const int16_t sine64[64] = {
	0,     1568,  3121,  4645,  6123,  7542,  8889,  10150,
	11314, 12368, 13304, 14111, 14782, 15311, 15693, 15923,
	16000, 15923, 15693, 15311, 14782, 14111, 13304, 12368,
	11314, 10150, 8889,  7542,  6123,  4645,  3121,  1568,
	0,     -1568, -3121, -4645, -6123, -7542, -8889, -10150,
	-11314, -12368, -13304, -14111, -14782, -15311, -15693, -15923,
	-16000, -15923, -15693, -15311, -14782, -14111, -13304, -12368,
	-11314, -10150, -8889,  -7542,  -6123,  -4645,  -3121,  -1568,
};

static int codec_wr(uint8_t addr, uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(i2c_dev, addr, reg, val);
}

static int codec_rd(uint8_t addr, uint8_t reg, uint8_t *val)
{
	return i2c_reg_read_byte(i2c_dev, addr, reg, val);
}

static uint8_t vol_to_reg(uint8_t percent)
{
	/* REG32 is linear in dB: 0x00 = -95.5 dB, 0xBF = 0 dB, 0xFF = +32 dB.
	 * Preserve the old mapping through 80 (80 = -8 dB), then extend the top
	 * of the 0..100 ABI scale to +6 dB. Hot mastered tracks can distort at
	 * the high end; no software gain is applied to the PCM samples. */
	int db10;
	int reg;

	if (percent == 0) {
		return 0;
	}
	db10 = percent <= 80 ? -400 + (int)percent * 4 :
		-80 + ((int)percent - 80) * 7;
	reg = (db10 * 2) / 10 + 191;
	if (reg < 1) {
		reg = 1;
	}
	if (reg > 0xCB) {
		reg = 0xCB;
	}
	return (uint8_t)reg;
}

static int es8311_apply_vol(void)
{
	int err = codec_wr(DAC_ADDR, 0x32, vol_to_reg(g_vol));
	if (err) {
		return err;
	}
	return codec_wr(DAC_ADDR, 0x31, g_vol == 0 ? 0x60 : 0x00);
}

/* Official 05-audio_es8311 open + start, clocks for 16 kHz @ MCLK=256*fs. */
static int es8311_dac_on(void)
{
	static const uint8_t tbl[][2] = {
		{0x00, 0x80}, /* slave, analog on, MCLK from pin */
		{0x01, 0x3F}, /* clocks on, MCLK pin */
		{0x09, 0x0C}, /* DAC SDP 16-bit I2S, port enabled */
		{0x0A, 0x0C},
		{0x0D, 0x01},
		{0x0E, 0x02},
		{0x12, 0x00}, /* DAC analog on */
		{0x14, 0x1A},
		{0x15, 0x40},
		{0x17, IS_ENABLED(CONFIG_ASTER_AUDIO_ADC_ES8311) ? 0xC8 : 0xBF},
		{0x37, 0x08},
		{0x45, 0x00},
	};
	int err = 0;

	for (size_t i = 0; i < ARRAY_SIZE(tbl); i++) {
		err = codec_wr(DAC_ADDR, tbl[i][0], tbl[i][1]);
		if (err) {
			return err;
		}
	}
	return es8311_apply_vol();
}

static int es8311_init(void)
{
	uint8_t id = 0;
	int err = codec_rd(DAC_ADDR, 0xFD, &id);

	if (err) {
		LOG_ERR("es8311 probe %d", err);
		return err;
	}
	if (id != 0x83) {
		LOG_WRN("es8311 id 0x%02x (expected 0x83)", id);
	}

	(void)codec_wr(DAC_ADDR, 0x00, 0x1F);
	k_msleep(10);
	(void)codec_wr(DAC_ADDR, 0x00, 0x00);
	(void)codec_wr(DAC_ADDR, 0x44, 0x08);
	(void)codec_wr(DAC_ADDR, 0x44, 0x08);

	static const uint8_t tbl[][2] = {
		{0x01, 0x30},
		{0x02, 0x00},
		{0x03, 0x10},
		{0x04, 0x10},
		{0x05, 0x00},
		{0x0B, 0x00},
		{0x0C, 0x00},
		{0x10, 0x1F}, /* analog / VMID up (0x03 left the DAC cold) */
		{0x11, 0x7F},
		{0x00, 0x80},
		{0x01, 0x3F},
		/* 16 kHz, MCLK = 256 * fs = 4.096 MHz (Zephyr i2s_esp32) */
		{0x02, 0x00},
		{0x05, 0x00},
		{0x03, 0x10},
		{0x04, 0x20},
		{0x07, 0x00},
		{0x08, 0xFF},
		{0x06, 0x03},
		{0x13, 0x10},
		{0x16, 0x24},
		{0x1B, 0x0A},
		{0x1C, 0x6A},
		{0x44, 0x08},
	};
	for (size_t i = 0; i < ARRAY_SIZE(tbl); i++) {
		err = codec_wr(DAC_ADDR, tbl[i][0], tbl[i][1]);
		if (err) {
			return err;
		}
	}
	err = es8311_dac_on();
	if (err) {
		return err;
	}
	LOG_INF("es8311 ready id=0x%02x", id);
	return 0;
}

static int es7210_init(void)
{
	uint8_t id = 0;
	uint8_t try_addr[2] = {ADC_ADDR, 0x40};
	int err = -ENODEV;

	for (int a = 0; a < 2; a++) {
		err = codec_rd(try_addr[a], 0x3D, &id);
		if (err == 0) {
			adc_addr = try_addr[a];
			break;
		}
	}
	if (err) {
		LOG_ERR("es7210 probe %d", err);
		return err;
	}
	if (id != 0x72) {
		LOG_WRN("es7210 id 0x%02x (expected 0x72) @0x%02x", id, adc_addr);
	}

	(void)codec_wr(adc_addr, 0x00, 0xFF);
	k_msleep(10);

	/*
	 * Register sequence from espressif/es7210 es7210_config_codec
	 * (LCKFB 04-audio_es7210). TDM is left off: Zephyr i2s_esp32 is
	 * STD Philips stereo, so MIC1/MIC2 map to L/R.
	 */
	static const uint8_t tbl[][2] = {
		{0x00, 0x32},
		{0x09, 0x30}, /* power-up timing */
		{0x0A, 0x30},
		{0x23, 0x2A}, /* ADC12 HPF */
		{0x22, 0x0A},
		{0x21, 0x2A}, /* ADC34 HPF */
		{0x20, 0x0A},
		{0x11, 0x60}, /* 16-bit I2S */
		{0x12, 0x00}, /* STD, not 1xFS TDM */
		{0x40, 0xC3}, /* analog / VMID */
		{0x41, 0x70}, /* MIC12 bias 2.87 V */
		{0x42, 0x70},
		{0x43, 0x1A}, /* PGA on + 30 dB */
		{0x44, 0x1A},
		{0x45, 0x1A},
		{0x46, 0x1A},
		{0x47, 0x08}, /* MIC1-4 analog on */
		{0x48, 0x08},
		{0x49, 0x08},
		{0x4A, 0x08},
		/* 16 kHz, MCLK = 256 * fs = 4.096 MHz */
		{0x07, 0x20},
		{0x02, 0xC1},
		{0x04, 0x01},
		{0x05, 0x00},
		{0x06, 0x04}, /* DLL off */
		{0x4B, 0x0F}, /* MIC12 bias / ADC / PGA on */
		{0x4C, 0x0F},
		{0x13, 0x00},
		{0x14, 0x00}, /* unmute */
		{0x15, 0x00},
		{0x1B, 0xBF}, /* 0 dB digital volume */
		{0x1C, 0xBF},
		{0x1D, 0xBF},
		{0x1E, 0xBF},
		{0x00, 0x71},
		{0x00, 0x41},
	};
	for (size_t i = 0; i < ARRAY_SIZE(tbl); i++) {
		err = codec_wr(adc_addr, tbl[i][0], tbl[i][1]);
		if (err) {
			return err;
		}
	}
	LOG_INF("es7210 ready id=0x%02x addr=0x%02x", id, adc_addr);
	return 0;
}

static void pa_set(bool on)
{
	if (!gpio_is_ready_dt(&pa_en)) {
		LOG_WRN("pa gpio not ready");
		return;
	}
	if (on == pa_on) {
		return;
	}
	int err = gpio_pin_set_dt(&pa_en, on ? 1 : 0);
	if (err) {
		LOG_ERR("pa %s %d", on ? "on" : "off", err);
		return;
	}
	pa_on = on;
	if (on) {
		k_msleep(30);
	}
}

static void play_wait(size_t bytes, int64_t t0)
{
	int64_t need = (int64_t)((bytes * 1000ull) /
				 (CONFIG_ASTER_AUDIO_RATE * ASTER_PLAT_AUDIO_FRAME)) + 80;
	int64_t left = need - (k_uptime_get() - t0);

	if (left > 0) {
		k_msleep((int32_t)left);
	}
}

static int codecs_init(void)
{
	int err;

	if (codecs_ready) {
		return 0;
	}
	i2c_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(aster_audio_i2c));
	i2s_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(aster_audio_i2s));
	if (!i2c_dev || !device_is_ready(i2c_dev) || !i2s_dev || !device_is_ready(i2s_dev)) {
		return -ENODEV;
	}
	if (gpio_is_ready_dt(&pa_en)) {
		(void)gpio_pin_configure_dt(&pa_en, GPIO_OUTPUT_INACTIVE);
		pa_on = false;
	}
	err = es8311_init();
	if (err) {
		return err;
	}
	err = IS_ENABLED(CONFIG_ASTER_AUDIO_ADC_ES8311) ? 0 : es7210_init();
	if (err) {
		LOG_WRN("es7210 init %d (playback still available)", err);
	}
	codecs_ready = true;
	return 0;
}

static void i2s_shutdown(void)
{
	if (!i2s_dev) {
		return;
	}
	if (active_dir) {
		(void)i2s_trigger(i2s_dev, active_dir, I2S_TRIGGER_DROP);
		k_msleep(2);
	}
	struct i2s_config cfg = {0};
	(void)i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
	(void)i2s_configure(i2s_dev, I2S_DIR_RX, &cfg);
	active_dir = 0;
	i2s_queued = 0;
}

static int i2s_apply(enum i2s_dir dir, struct k_mem_slab *slab)
{
	struct i2s_config cfg = {
		.word_size = CONFIG_ASTER_AUDIO_BITS,
		.channels = CONFIG_ASTER_AUDIO_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER,
		.frame_clk_freq = dir == I2S_DIR_TX ? output_rate : CONFIG_ASTER_AUDIO_RATE,
		.mem_slab = slab,
		.block_size = dir == I2S_DIR_TX ? output_block : AUDIO_BLOCK,
		.timeout = AUDIO_TIMEOUT,
	};
	int err = i2s_configure(i2s_dev, dir, &cfg);
	if (err) {
		LOG_ERR("i2s configure dir=%d %d", (int)dir, err);
	}
	return err;
}

/* ES8311 MCLK=256fs clock ratios, matching Espressif's coefficient table.
 * 22.05..48k DAC OSR is 0x10; 8..16k uses 0x20. I2S supplies MCLK/BCLK. */
static int i2s_setup_rate(enum i2s_dir dir, uint32_t rate)
{
    if (active_dir) i2s_shutdown();
    output_rate = rate;
    output_block = audio_rate_block(rate);
    int err = codec_wr(DAC_ADDR, 0x04, rate > 16000 ? 0x10 : 0x20);
    if (err) return err;
    err = i2s_apply(dir, dir == I2S_DIR_TX ? &tx_slab : &rx_slab);
    if (err) return err;
    active_dir = dir;
    i2s_queued = 0;
    return 0;
}
static int i2s_setup(enum i2s_dir dir)
{
    return i2s_setup_rate(dir, CONFIG_ASTER_AUDIO_RATE);
}

/* START only after two blocks are queued: one is pulled into DMA, the
 * other covers the ~8 ms block time so the first decode cannot underrun. */
static int i2s_kick(bool *started, bool force)
{
	int err;

	if (*started) {
		return 0;
	}
	if (!force && i2s_queued < 2) {
		return 0;
	}
	if (i2s_queued < 1) {
		return 0;
	}
	err = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (err) {
		LOG_WRN("i2s start %d", err);
		return err;
	}
	*started = true;
	return 0;
}

static int i2s_submit(void *blk, bool *started)
{
	/* Mix every active effect with the background before saturating once.
	 * Separate voices let a movement cue finish underneath an eat cue. */
	k_mutex_lock(&effect_mu, K_FOREVER);
	int16_t *dst = blk;
    for (size_t i = 0; i < output_block / ASTER_PLAT_AUDIO_FRAME; ++i) {
        int32_t l = dst[2*i], r = dst[2*i+1];
        for (int slot = 0; slot < EFFECT_SLOTS; ++slot) {
            if (!effects[slot].playing) continue;
            size_t frame = effects[slot].off / ASTER_PLAT_AUDIO_FRAME;
            size_t frames = effects[slot].size / ASTER_PLAT_AUDIO_FRAME;
            const int16_t *pcm = (const int16_t *)effects[slot].pcm;
            l += audio_rate_sample(pcm, frames, 2, 0, frame, effects[slot].fraction, output_rate);
            r += audio_rate_sample(pcm, frames, 2, 1, frame, effects[slot].fraction, output_rate);
            audio_rate_advance(&frame, &effects[slot].fraction, CONFIG_ASTER_AUDIO_RATE, output_rate);
            effects[slot].off = MIN(frame * ASTER_PLAT_AUDIO_FRAME, effects[slot].size);
            if (effects[slot].off == effects[slot].size) effects[slot].playing = false;
        }
        if (ui_voice.pcm) {
            int32_t v = audio_rate_sample(ui_voice.pcm, ui_voice.frames, 1, 0,
                                         ui_voice.off, ui_voice.fraction, output_rate);
            v = v * (int32_t)ui_voice.gain / 1000;
            l += v; r += v;
            audio_rate_advance(&ui_voice.off, &ui_voice.fraction, CONFIG_ASTER_AUDIO_RATE, output_rate);
            if (ui_voice.off >= ui_voice.frames) ui_voice.pcm = NULL;
        }
        dst[2*i] = (int16_t)CLAMP(l, -32768, 32767);
        dst[2*i+1] = (int16_t)CLAMP(r, -32768, 32767);
    }
	k_mutex_unlock(&effect_mu);
	int err = i2s_write(i2s_dev, blk, output_block);

	if (err) {
		k_mem_slab_free(&tx_slab, blk);
		return err;
	}
	if (i2s_queued < 255) {
		i2s_queued++;
	}
	return i2s_kick(started, false);
}

static int i2s_recover(bool *started)
{
	(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
	*started = false;
	i2s_queued = 0;
	return 0;
}

static int feed_silence(bool *started)
{
	void *blk = NULL;
	int err = k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(AUDIO_TIMEOUT));

	if (err) {
		return err;
	}
	memset(blk, 0, output_block);
	return i2s_submit(blk, started);
}

/* Board BCLK/WS are I2S TX pins. Keep TX running so the ADC sees clocks. */
static int tx_silence(void)
{
	void *blk = NULL;
	int err = k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(AUDIO_TIMEOUT));

	if (err) {
		return err;
	}
	memset(blk, 0, AUDIO_BLOCK);
	err = i2s_write(i2s_dev, blk, AUDIO_BLOCK);
	if (err) {
		k_mem_slab_free(&tx_slab, blk);
	}
	return err;
}

static bool sess_busy(void)
{
	bool busy;

	k_mutex_lock(&sess_mu, K_FOREVER);
	busy = sess.state != ASTER_AUDIO_IDLE;
	k_mutex_unlock(&sess_mu);
	return busy;
}

static void sess_clear_locked(void)
{
	if (sess.from_stream) {
		input_stream.error = sess.stop ? -ECANCELED : sess.err;
		input_stream.state = input_stream.error ? ASTER_AUDIO_STREAM_FAILED : ASTER_AUDIO_STREAM_DONE;
		aster_psram_free(input_stream.ring.data);
		memset(&input_stream.ring, 0, sizeof(input_stream.ring));
	}
	if (sess.file_open) {
		fs_close(&sess_file);
		sess.file_open = false;
	}
	if (sess.buf) {
		aster_psram_free(sess.buf);
		sess.buf = NULL;
	}
	sess.size = 0;
	sess.off = 0;
	sess.win_len = 0;
	sess.win_off = 0;
	sess.file_base = 0;
	sess.pcm_done = 0;
	sess.pcm_rate = CONFIG_ASTER_AUDIO_RATE;
	sess.msec_total = 0;
	sess.seek_msec = 0;
	sess.pause = false;
	sess.stop = false;
	sess.mp3 = false;
	sess.from_file = false;
	sess.from_stream = false;
	sess.effect_only = false;
	sess.effect_closing = false;
	sess.seek_pending = false;
	sess.state = ASTER_AUDIO_IDLE;
}

static bool mp3_hdr_ok(const uint8_t *h)
{
	unsigned layer;
	unsigned ver;
	unsigned br;
	unsigned sr;

	if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
		return false;
	}
	ver = (h[1] >> 3) & 3u;
	layer = (h[1] >> 1) & 3u;
	br = h[2] >> 4;
	sr = (h[2] >> 2) & 3u;
	if (ver == 1u || layer != 1u || br == 0xFu || sr == 3u) {
		return false;
	}
	return true;
}

static uint32_t pcm_bytes_to_msec(size_t bytes)
{
	return (uint32_t)((uint64_t)bytes * 1000u /
			  ((uint64_t)CONFIG_ASTER_AUDIO_RATE * ASTER_PLAT_AUDIO_FRAME));
}

static uint32_t mp3_msec_est(const uint8_t *h, size_t payload)
{
	static const uint16_t br_v1l3[16] = {
		0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
	};
	uint16_t kbps = 128;

	if (h && mp3_hdr_ok(h)) {
		uint16_t v = br_v1l3[h[2] >> 4];

		if (v) {
			kbps = v;
		}
	}
	if (!payload) {
		return 0;
	}
	return (uint32_t)((payload * 8u) / kbps);
}

static uint32_t sess_msec_now(void)
{
	uint32_t ms = (uint32_t)((uint64_t)sess.pcm_done * 1000u / (sess.pcm_rate ? sess.pcm_rate : CONFIG_ASTER_AUDIO_RATE));

	if (sess.msec_total && ms > sess.msec_total) {
		ms = sess.msec_total;
	}
	return ms;
}

bool aster_plat_audio_is_mp3(const uint8_t *buf, size_t len)
{
	size_t i;

	if (!buf || len < 4) {
		return false;
	}
	if (len >= 3 && buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {
		return true;
	}
	i = 0;
	while (i + 4 <= len && i < 64 && buf[i] == 0) {
		i++;
	}
	if (i + 4 > len) {
		return false;
	}
	return mp3_hdr_ok(buf + i);
}

static size_t id3_skip(const uint8_t *p, size_t n)
{
	size_t len;
	size_t total;

	if (n < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3') {
		return 0;
	}
	len = aster_synchsafe32(p + 6);
	total = 10 + len;
	if (p[5] & 0x10) {
		total += 10;
	}
	return total > n ? n : total;
}

/* Uncapped ID3 size so a file player can seek past a large tag. */
static size_t id3_total(const uint8_t *p, size_t n)
{
	size_t len;
	size_t total;

	if (n < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3') {
		return 0;
	}
	len = aster_synchsafe32(p + 6);
	total = 10 + len;
	if (p[5] & 0x10) {
		total += 10;
	}
	return total;
}

static void mp3_reset(void)
{
	mp3dec_init(&mp3p.dec);
	mp3p.fifo_n = 0;
	mp3p.hz = 0;
	mp3p.ch = 0;
	mp3p.wrote = 0;
}

static int fifo_write_block(bool *started)
{
	void *blk = NULL;
	unsigned n = output_block / sizeof(int16_t);
	int err;

	if (mp3p.fifo_n < n) {
		return 0;
	}
	err = k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(AUDIO_TIMEOUT));
	if (err) {
		return err;
	}
	memcpy(blk, mp3p.fifo, output_block);
	mp3p.fifo_n -= n;
	if (mp3p.fifo_n) {
		memmove(mp3p.fifo, mp3p.fifo + n, mp3p.fifo_n * sizeof(int16_t));
	}
	err = i2s_submit(blk, started);
	if (err == 0) {
		mp3p.wrote++;
	}
	return err;
}

static int fifo_flush_blocks(bool *started)
{
	int err = 0;

	while (mp3p.fifo_n >= (output_block / sizeof(int16_t))) {
		err = fifo_write_block(started);
		if (err) {
			return err;
		}
	}
	return 0;
}

static int fifo_drain(bool *started)
{
	int err = fifo_flush_blocks(started);

	if (err || mp3p.fifo_n == 0) {
		return err;
	}
	while (mp3p.fifo_n < (output_block / sizeof(int16_t))) {
		mp3p.fifo[mp3p.fifo_n++] = 0;
	}
	err = fifo_write_block(started);
	if (err) {
		return err;
	}
	return i2s_kick(started, true);
}

static int fifo_push_stereo(int16_t l, int16_t r, bool *started)
{
	if (mp3p.fifo_n + 2 > MP3_FIFO_SAMP) {
		int err = fifo_flush_blocks(started);

		if (err) {
			return err;
		}
		if (mp3p.fifo_n + 2 > MP3_FIFO_SAMP) {
			return -ENOMEM;
		}
	}
	mp3p.fifo[mp3p.fifo_n++] = l;
	mp3p.fifo[mp3p.fifo_n++] = r;
	sess.pcm_done++;
	return 0;
}

static int output_mp3_frame(const int16_t *src, int samples, int ch, int hz,
			  bool *started)
{
    if (samples <= 0 || ch < 1 || ch > 2 || !audio_mp3_rate(hz)) return -EPROTO;
    if (!mp3p.hz) {
        if (*started && output_rate != (uint32_t)hz) return -EPROTO;
        int err = output_rate == (uint32_t)hz ? 0 : i2s_setup_rate(I2S_DIR_TX, (uint32_t)hz);
        if (err) return err;
        k_mutex_lock(&sess_mu, K_FOREVER);
        sess.pcm_rate = (uint32_t)hz;
        k_mutex_unlock(&sess_mu);
        LOG_INF("MP3 native output: %d Hz %d channels, I2S block=%u", hz, ch, (unsigned)output_block);
    } else if (mp3p.hz != hz) {
        /* A stream cannot change clocks after DMA starts; reject safely. */
        return -EPROTO;
    }
    mp3p.hz = hz;
    mp3p.ch = ch;
    for (int i = 0; i < samples; ++i) {
        int16_t l = src[ch == 1 ? i : 2*i];
        int16_t r = src[ch == 1 ? i : 2*i+1];
        int err = fifo_push_stereo(l, r, started);
        if (err) return err;
    }
    return 0;
}

/* Decode at most one MP3 frame into the PCM fifo, then push full I2S blocks.
 * *off is the compressed byte cursor. Returns 1 when the stream is finished.
 * eof=false means `buf` is a refillable window — do not drain on a short tail. */
static int mp3_pump(const uint8_t *buf, size_t size, size_t *off, bool *started,
		    bool eof)
{
	mp3dec_frame_info_t info;
	int samples;
	int err;
	size_t left;

	if (*off >= size) {
		if (!eof) {
			return 0;
		}
		err = fifo_drain(started);
		return err ? err : 1;
	}

	memset(&info, 0, sizeof(info));
	samples = mp3dec_decode_frame(&mp3p.dec, buf + *off, (int)(size - *off),
				      mp3p.frame, &info);
	if (samples > 0 && info.frame_bytes > 0) {
		*off += (size_t)info.frame_bytes;
		err = output_mp3_frame(mp3p.frame, samples, info.channels, info.hz,
				     started);
		if (err) {
			return err;
		}
		return fifo_flush_blocks(started);
	}
	if (info.frame_bytes > 0) {
		*off += (size_t)info.frame_bytes;
		return 0;
	}
	left = size - *off;
	if (left < 4) {
		if (!eof) {
			return 0;
		}
		err = fifo_drain(started);
		return err ? err : 1;
	}
	if (!eof && left < MP3_NEED) {
		if (*off == 0 && size >= STREAM_WIN) {
			(*off)++;
		}
		return 0;
	}
	(*off)++;
	return 0;
}

static int stream_refill(bool *eof)
{
	size_t space;
	int n;

	*eof = false;
	if (!sess.from_file || !sess.buf) {
		return 0;
	}
	if (sess.win_off) {
		if (sess.win_len > sess.win_off) {
			memmove(sess.buf, sess.buf + sess.win_off,
				sess.win_len - sess.win_off);
			sess.win_len -= sess.win_off;
		} else {
			sess.win_len = 0;
		}
		sess.win_off = 0;
	}
	space = STREAM_WIN - sess.win_len;
	if (space == 0) {
		return 0;
	}
	n = aster_fs_read_safe(&sess_file, sess.buf + sess.win_len, space);
	if (n < 0) {
		return n;
	}
	if (n == 0) {
		*eof = true;
	}
	sess.win_len += (size_t)n;
	return 0;
}

/* Worker-only contiguous window, fed from a separately synchronized APP ring.
 * No filesystem or network call, and no wait while holding audio_lock. */
static int input_refill(bool *eof, bool *waiting)
{
    if (sess.win_off) {
        memmove(sess.buf, sess.buf + sess.win_off, sess.win_len - sess.win_off);
        sess.win_len -= sess.win_off;
        sess.win_off = 0;
    }
    k_mutex_lock(&sess_mu, K_FOREVER);
    sess.win_len += audio_ring_read(&input_stream.ring, sess.buf + sess.win_len,
                                    STREAM_WIN - sess.win_len);
    *eof = input_stream.eof && !input_stream.ring.used;
    k_mutex_unlock(&sess_mu);
    if (!input_stream.id3_checked) {
        if (sess.win_len < 10 && !*eof) { *waiting = true; return 0; }
        input_stream.id3_checked = true;
        if (sess.win_len >= 10 && !memcmp(sess.buf, "ID3", 3)) {
            if (sess.buf[3] < 2 || sess.buf[3] > 4 ||
                ((sess.buf[6]|sess.buf[7]|sess.buf[8]|sess.buf[9]) & 0x80)) return -EPROTO;
            input_stream.id3_left = 10 + aster_synchsafe32(sess.buf + 6);
            if (sess.buf[3] == 4 && (sess.buf[5] & 0x10)) input_stream.id3_left += 10;
            if (input_stream.id3_left > 1024u * 1024u) return -EMSGSIZE;
        }
    }
    if (input_stream.id3_left) {
        size_t n = MIN(input_stream.id3_left, sess.win_len);
        sess.win_off += n;
        input_stream.id3_left -= n;
        k_mutex_lock(&sess_mu, K_FOREVER);
        sess.off += n;
        input_stream.consumed = sess.off;
        k_mutex_unlock(&sess_mu);
        if (input_stream.id3_left && *eof) return -EPROTO;
        *waiting = true;
        return 0;
    }
    k_mutex_lock(&sess_mu, K_FOREVER);
    size_t ready = sess.win_len - sess.win_off;
    if (!input_stream.buffering && ready < MP3_NEED && !*eof) {
        input_stream.buffering = true;
        if (mp3p.hz) ++input_stream.underruns;
    }
    if (input_stream.buffering && (ready + input_stream.ring.used >= 24576 || *eof)) input_stream.buffering = false;
    *waiting = input_stream.buffering;
    k_mutex_unlock(&sess_mu);
    return 0;
}

static int mp3_align_sync(void)
{
	int tries = 0;
	bool eof = false;

	if (!sess.buf || !sess.from_file) {
		return 0;
	}
	sess.win_off = 0;
	while (tries++ < 12) {
		int err = stream_refill(&eof);
		size_t i;

		if (err) {
			return err;
		}
		for (i = 0; i + 4 <= sess.win_len; i++) {
			if (mp3_hdr_ok(sess.buf + i)) {
				if (i) {
					memmove(sess.buf, sess.buf + i,
						sess.win_len - i);
					sess.win_len -= i;
					sess.off += i;
				}
				sess.win_off = 0;
				return 0;
			}
		}
		if (sess.win_len >= 3) {
			size_t keep = 3;

			sess.off += sess.win_len - keep;
			memmove(sess.buf, sess.buf + sess.win_len - keep, keep);
			sess.win_len = keep;
		} else {
			sess.off += sess.win_len;
			sess.win_len = 0;
		}
		sess.win_off = 0;
		if (eof) {
			return 0;
		}
	}
	return 0;
}

static int feed_one_file(bool *started)
{
	void *blk = NULL;
	size_t n;
	int r;
	int err = k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(AUDIO_TIMEOUT));

	if (err) {
		return err;
	}
	n = sess.size - sess.off;
	if (n > AUDIO_BLOCK) {
		n = AUDIO_BLOCK;
	}
	r = aster_fs_read_safe(&sess_file, blk, n);
	if (r < 0) {
		k_mem_slab_free(&tx_slab, blk);
		return r;
	}
	if (r == 0) {
		k_mem_slab_free(&tx_slab, blk);
		return 1;
	}
	if ((size_t)r < AUDIO_BLOCK) {
		memset((uint8_t *)blk + r, 0, AUDIO_BLOCK - (size_t)r);
	}
	sess.off += (size_t)r;
	sess.pcm_done += (uint32_t)((size_t)r / ASTER_PLAT_AUDIO_FRAME);
	err = i2s_submit(blk, started);
	if (err) {
		return err;
	}
	if (sess.off >= sess.size) {
		err = i2s_kick(started, true);
		if (err) {
			return err;
		}
		return 1;
	}
	return 0;
}

static int wav_data_range(struct fs_file_t *f, size_t fsz, size_t *data_off,
			  size_t *data_len)
{
	uint8_t h[12];
	size_t pos;
	int n;

	n = aster_fs_read_safe(f, h, 12);
	if (n < 12) {
		return -EIO;
	}
	if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
		*data_off = 0;
		*data_len = fsz;
		return 0;
	}
	pos = 12;
	while (pos + 8 <= fsz) {
		uint8_t ch[8];
		uint32_t csz;

		if (fs_seek(f, (off_t)pos, FS_SEEK_SET) < 0) {
			return -EIO;
		}
		n = aster_fs_read_safe(f, ch, 8);
		if (n < 8) {
			return -EIO;
		}
		csz = aster_le32(ch + 4);
		if (memcmp(ch, "data", 4) == 0) {
			*data_off = pos + 8;
			*data_len = csz;
			if (*data_off > fsz) {
				return -EINVAL;
			}
			if (*data_off + *data_len > fsz) {
				*data_len = fsz - *data_off;
			}
			return 0;
		}
		pos += 8u + ((csz + 1u) & ~1u);
	}
	return -EINVAL;
}

static int feed_one(const uint8_t *pcm, size_t size, size_t *off, bool *started)
{
	void *blk = NULL;
	int err = k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(AUDIO_TIMEOUT));

	if (err) {
		return err;
	}
	size_t n = size - *off;
	if (n > AUDIO_BLOCK) {
		n = AUDIO_BLOCK;
	}
	memcpy(blk, pcm + *off, n);
	if (n < AUDIO_BLOCK) {
		memset((uint8_t *)blk + n, 0, AUDIO_BLOCK - n);
	}
	err = i2s_submit(blk, started);
	if (err) {
		return err;
	}
	*off += n;
	sess.pcm_done += (uint32_t)(n / ASTER_PLAT_AUDIO_FRAME);
	return 0;
}

static int apply_seek_io(uint32_t msec)
{
	size_t off;

	if (sess.size == 0) {
		return -EINVAL;
	}
	if (sess.msec_total == 0) {
		off = 0;
	} else {
		if (msec > sess.msec_total) {
			msec = sess.msec_total;
		}
		off = (size_t)((uint64_t)msec * sess.size / sess.msec_total);
	}
	if (!sess.mp3) {
		off -= off % ASTER_PLAT_AUDIO_FRAME;
	}
	if (off > sess.size) {
		off = sess.size;
	}
	if (sess.from_file && sess.file_open) {
		if (fs_seek(&sess_file, (off_t)(sess.file_base + off), FS_SEEK_SET) != 0) {
			return -EIO;
		}
	}
	sess.off = off;
	sess.win_len = 0;
	sess.win_off = 0;
	sess.pcm_done = (uint32_t)((uint64_t)msec * (sess.pcm_rate ? sess.pcm_rate : CONFIG_ASTER_AUDIO_RATE) / 1000u);
	if (sess.mp3) {
		mp3_reset();
		(void)mp3_align_sync();
	}
	return 0;
}

static void player_loop(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		/* Caller holds play_life across the give, so a token posted
		 * for this thread is visible once the lock is acquired. An
		 * empty semaphore means the session is over: drop the stack. */
		k_mutex_lock(&play_life, K_FOREVER);
		if (k_sem_take(&play_go, K_NO_WAIT) != 0) {
			play_alive = false;
			k_mutex_unlock(&play_life);
			return;
		}
		k_mutex_unlock(&play_life);

		k_mutex_lock(&sess_mu, K_FOREVER);
		uint8_t *buf = sess.buf;
		size_t size = sess.size;
		bool from_file = sess.from_file;
		bool from_stream = sess.from_stream;
		bool is_mp3 = sess.mp3;
		bool effect_only = sess.effect_only;
		if (sess.stop || size == 0 ||
		    (from_file && !sess.file_open) ||
		    (!from_file && !buf && !effect_only)) {
			sess_clear_locked();
			k_mutex_unlock(&sess_mu);
			continue;
		}

		if (!from_file) {
			sess.off = 0;
		}
		sess.state = ASTER_AUDIO_PLAYING;
		k_mutex_unlock(&sess_mu);

		if (is_mp3) {
			mp3_reset();
			if (!from_file && !from_stream) {
				k_mutex_lock(&sess_mu, K_FOREVER);
				sess.off = id3_skip(buf, size);
				k_mutex_unlock(&sess_mu);
			} else {
				sess.win_len = 0;
				sess.win_off = 0;
			}
		}

		k_mutex_lock(&audio_lock, K_FOREVER);
		int err = codecs_init();
		if (err == 0) {
			err = es8311_dac_on();
		}
		if (err == 0) {
			pa_set(true);
			err = i2s_setup(I2S_DIR_TX);
		}
		k_mutex_unlock(&audio_lock);
		if (err) {
			LOG_ERR("player start %d", err);
			k_mutex_lock(&sess_mu, K_FOREVER);
			sess.err = err;
			sess_clear_locked();
			k_mutex_unlock(&sess_mu);
			continue;
		}

		bool started = false;
		int recovered = 0;
		uint32_t effect_idle_blocks = 0;
		while (true) {
			k_mutex_lock(&sess_mu, K_FOREVER);
			if (sess.stop) {
				k_mutex_unlock(&sess_mu);
				break;
			}
			if (sess.seek_pending) {
				uint32_t ms = sess.seek_msec;

				sess.seek_pending = false;
				k_mutex_unlock(&sess_mu);
				k_mutex_lock(&audio_lock, K_FOREVER);
				if (started) {
					(void)feed_silence(&started);
					(void)feed_silence(&started);
				}
				k_mutex_unlock(&audio_lock);
				k_mutex_lock(&sess_mu, K_FOREVER);
				(void)apply_seek_io(ms);
				k_mutex_unlock(&sess_mu);
				/* Leave I2S running. DROP/reconfigure here
				 * races the DMA callback into ERROR, after
				 * which write returns -EIO until a hard reset. */
				continue;
			}
			if (sess.pause) {
				sess.state = ASTER_AUDIO_PAUSED;
				k_mutex_unlock(&sess_mu);
				k_mutex_lock(&audio_lock, K_FOREVER);
				i2s_shutdown();
				started = false;
				k_mutex_unlock(&audio_lock);
				for (;;) {
					k_mutex_lock(&sess_mu, K_FOREVER);
					bool pausing = sess.pause;
					bool stopping = sess.stop;
					k_mutex_unlock(&sess_mu);
					if (!pausing || stopping) {
						break;
					}
					k_sem_take(&play_wake, K_FOREVER);
				}
				k_mutex_lock(&sess_mu, K_FOREVER);
				bool stopping = sess.stop;
				if (!stopping) {
					sess.state = ASTER_AUDIO_PLAYING;
				}
				k_mutex_unlock(&sess_mu);
				if (stopping) {
					break;
				}
				k_mutex_lock(&audio_lock, K_FOREVER);
				(void)es8311_dac_on();
				pa_set(true);
				(void)i2s_setup_rate(I2S_DIR_TX, output_rate);
				k_mutex_unlock(&audio_lock);
				continue;
			}
			size_t off = sess.off;
			bool waiting_input = false;
			k_mutex_unlock(&sess_mu);
			if (!from_file && !from_stream && !effect_only && off >= size && !(is_mp3 && mp3p.fifo_n)) {
				break;
			}
			k_mutex_lock(&audio_lock, K_FOREVER);
			if (effect_only) {
				err = feed_silence(&started);
				k_mutex_lock(&effect_mu, K_FOREVER);
				bool active = ui_voice.pcm != NULL;
				for (int i = 0; i < EFFECT_SLOTS; i++) {
					active |= effects[i].playing;
				}
				k_mutex_unlock(&effect_mu);
				effect_idle_blocks = active ? 0 : effect_idle_blocks + 1;
				if (err == 0 && effect_idle_blocks > EFFECT_IDLE_BLOCKS) {
					/* Mark the closing interval under the same locks as play.
					 * A concurrent cue waits for IDLE and starts a new session. */
					k_mutex_lock(&sess_mu, K_FOREVER);
					k_mutex_lock(&effect_mu, K_FOREVER);
					active = ui_voice.pcm != NULL;
					for (int i = 0; i < EFFECT_SLOTS; i++) {
						active |= effects[i].playing;
					}
					if (!active && !sess.stop) {
						sess.effect_closing = true;
						err = 1;
					} else {
						effect_idle_blocks = 0;
					}
					k_mutex_unlock(&effect_mu);
					k_mutex_unlock(&sess_mu);
				}
			} else if (from_stream) {
                bool eof = false;
                mp3p.wrote = 0;
                err = input_refill(&eof, &waiting_input);
                if (!err && !waiting_input) {
                    size_t before = sess.win_off;
                    err = mp3_pump(sess.buf, sess.win_len, &sess.win_off, &started, eof);
                    off = sess.off + sess.win_off - before;
                    if (err > 0 && !mp3p.hz) err = -EPROTO;
                } else off = sess.off;
                if (!err && started && !mp3p.wrote) err = feed_silence(&started);
			} else if (from_file && is_mp3) {
				size_t before;
				bool eof = false;

				mp3p.wrote = 0;
				err = stream_refill(&eof);
				if (err) {
					k_mutex_unlock(&audio_lock);
					LOG_WRN("player refill %d", err);
					break;
				}
				before = sess.win_off;
				err = mp3_pump(sess.buf, sess.win_len, &sess.win_off,
					       &started, eof);
				off = sess.off + (sess.win_off - before);
				if (err == 0 && started && mp3p.wrote == 0) {
					err = feed_silence(&started);
				}
			} else if (from_file) {
				err = feed_one_file(&started);
				off = sess.off;
			} else if (is_mp3) {
				mp3p.wrote = 0;
				err = mp3_pump(buf, size, &off, &started, true);
				if (err == 0 && started && mp3p.wrote == 0) {
					err = feed_silence(&started);
				}
			} else {
				err = feed_one(buf, size, &off, &started);
			}
			if (err > 0) {
				int k = i2s_kick(&started, true);

				if (k) {
					err = k;
				}
			}
			k_mutex_unlock(&audio_lock);
			k_mutex_lock(&sess_mu, K_FOREVER);
			sess.off = off;
            if (from_stream) {
                input_stream.consumed = off;
                input_stream.played_ms = (uint32_t)((uint64_t)sess.pcm_done * 1000u / (sess.pcm_rate ? sess.pcm_rate : CONFIG_ASTER_AUDIO_RATE));
            }
			k_mutex_unlock(&sess_mu);
            if (waiting_input && !started && !err) k_sem_take(&play_wake, K_MSEC(20));
			if (err > 0) {
				break; /* mp3 stream finished */
			}
			if (err == -EIO && recovered < 3) {
				recovered++;
				LOG_WRN("player feed -EIO, prepare I2S (%u)", recovered);
				k_mutex_lock(&audio_lock, K_FOREVER);
				(void)i2s_recover(&started);
				k_mutex_unlock(&audio_lock);
				continue;
			}
			if (err) {
				LOG_WRN("player feed %d", err);
				break;
			}
			recovered = 0;
		}

		k_mutex_lock(&audio_lock, K_FOREVER);
		if (started) {
			(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
			k_msleep(40);
		}
		i2s_shutdown();
		pa_set(false);
		k_mutex_unlock(&audio_lock);

		k_mutex_lock(&sess_mu, K_FOREVER);
		if (err < 0) {
			sess.err = err;
		}
		sess_clear_locked();
		k_mutex_unlock(&sess_mu);
	}
}

static int wake_player(void)
{
	k_mutex_lock(&play_life, K_FOREVER);
	if (!play_alive) {
		/* The exiting player drops play_life before it returns, so
		 * joining while this lock is held cannot deadlock. */
		if (play_join) {
			k_thread_join(&play_thread, K_FOREVER);
			play_join = false;
		}
		aster_dram_free(play_stack);
		play_stack = aster_dram_alloc(K_KERNEL_STACK_LEN(AUDIO_STACK));
		if (!play_stack) {
			k_mutex_unlock(&play_life);
			return -ENOMEM;
		}
		play_alive = true;
		play_join = true;
		k_thread_create(&play_thread, play_stack,
				K_KERNEL_STACK_LEN(AUDIO_STACK) - K_KERNEL_STACK_RESERVED,
				player_loop, NULL, NULL, NULL,
				K_PRIO_PREEMPT(6), K_FP_REGS, K_NO_WAIT);
		k_thread_name_set(&play_thread, "aster_aplay");
	}
	k_sem_give(&play_go);
	k_mutex_unlock(&play_life);
	return 0;
}

static int play_blocks(const uint8_t *pcm, size_t size)
{
	int err = es8311_dac_on();
	if (err) {
		return err;
	}
	err = i2s_setup(I2S_DIR_TX);
	if (err) {
		return err;
	}

	int64_t t0 = k_uptime_get();
	size_t off = 0;
	bool started = false;
	while (off < size) {
		void *blk = NULL;
		err = k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(AUDIO_TIMEOUT));
		if (err) {
			break;
		}
		size_t n = size - off;
		if (n > AUDIO_BLOCK) {
			n = AUDIO_BLOCK;
		}
		memcpy(blk, pcm + off, n);
		if (n < AUDIO_BLOCK) {
			memset((uint8_t *)blk + n, 0, AUDIO_BLOCK - n);
		}
		err = i2s_write(i2s_dev, blk, AUDIO_BLOCK);
		if (err) {
			k_mem_slab_free(&tx_slab, blk);
			break;
		}
		off += n;
		if (!started) {
			err = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
			if (err) {
				break;
			}
			started = true;
		}
	}
	if (started) {
		(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
		play_wait(off, t0);
	} else if (err == 0) {
		err = -EIO;
	}
	i2s_shutdown();
	return err;
}

static int play_mp3_blocks(const uint8_t *buf, size_t size)
{
	size_t off = id3_skip(buf, size);
	bool started = false;
	int err;

	mp3_reset();
	err = es8311_dac_on();
	if (err) {
		return err;
	}
	err = i2s_setup(I2S_DIR_TX);
	if (err) {
		return err;
	}
	while (true) {
		err = mp3_pump(buf, size, &off, &started, true);
		if (err > 0) {
			err = 0;
			break;
		}
		if (err) {
			break;
		}
	}
	if (started) {
		(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
		k_msleep(80);
	} else if (err == 0) {
		err = -EIO;
	}
	i2s_shutdown();
	return err;
}

static int record_blocks(uint8_t *pcm, size_t size)
{
	int32_t peak = 0;
	int err;

	i2s_shutdown();
	/* TX first: RX then becomes clock slave and shares BCK/WS. */
	err = i2s_setup(I2S_DIR_TX);
	if (err) {
		return err;
	}
	err = i2s_apply(I2S_DIR_RX, &rx_slab);
	if (err) {
		i2s_shutdown();
		return err;
	}
	active_dir = I2S_DIR_BOTH;

	for (int i = 0; i < AUDIO_NBLOCKS - 2; i++) {
		err = tx_silence();
		if (err) {
			/* Used to return silently: this is where a drained tx_slab
			 * shows up, as -EAGAIN from k_mem_slab_alloc(). */
			LOG_ERR("record: tx_silence[%d] %d", i, err);
			i2s_shutdown();
			return err;
		}
	}
	err = i2s_trigger(i2s_dev, I2S_DIR_BOTH, I2S_TRIGGER_START);
	if (err) {
		LOG_ERR("i2s duplex start %d", err);
		i2s_shutdown();
		return err;
	}

	/* Drop a couple of blocks while the ADC analog path settles. */
	for (int i = 0; i < 2; i++) {
		void *blk = NULL;
		size_t got = 0;
		if (i2s_read(i2s_dev, &blk, &got) == 0 && blk) {
			k_mem_slab_free(&rx_slab, blk);
		}
		(void)tx_silence();
	}

	size_t off = 0;
	while (off < size) {
		void *blk = NULL;
		size_t got = 0;
		err = i2s_read(i2s_dev, &blk, &got);
		if (err || !blk) {
			/* Otherwise the only trace is the caller's trailing -EIO:
			 * say how far the capture got and what killed it. */
			LOG_ERR("record: i2s read %d got=%u off=%u", err, (unsigned)got,
				(unsigned)off);
			break;
		}
		size_t n = size - off;
		if (n > got) {
			n = got;
		}
		if (n > AUDIO_BLOCK) {
			n = AUDIO_BLOCK;
		}
		memcpy(pcm + off, blk, n);
		const int16_t *s = blk;
		size_t ns = n / sizeof(int16_t);
		for (size_t i = 0; i < ns; i++) {
			int32_t v = s[i];
			if (v < 0) {
				v = -v;
			}
			if (v > 32767) {
				v = 32767;
			}
			if (v > peak) {
				peak = v;
			}
		}
		k_mem_slab_free(&rx_slab, blk);
		off += n;
		(void)tx_silence();
	}
	i2s_shutdown();
	LOG_INF("record %u/%u bytes peak=%d", (unsigned)off, (unsigned)size, (int)peak);
	if (err) {
		return err;
	}
	return (off >= size) ? 0 : -EIO;
}

/* Raw PCM, recording and effect asset format. MP3 playback uses the decoded
 * sample rate privately; never expose a transient music rate as the format
 * required for the next raw-PCM call. */
int aster_plat_audio_info(uint32_t *rate, uint32_t *channels, uint32_t *bits)
{
	if (rate) {
		*rate = CONFIG_ASTER_AUDIO_RATE;
	}
	if (channels) {
		*channels = CONFIG_ASTER_AUDIO_CHANNELS;
	}
	if (bits) {
		*bits = CONFIG_ASTER_AUDIO_BITS;
	}
	return 0;
}

int aster_plat_audio_probe(uint8_t *dac_id, uint8_t *adc_id_out)
{
	k_mutex_lock(&audio_lock, K_FOREVER);
	int err = codecs_init();
	if (dac_id) {
		*dac_id = 0;
		if (i2c_dev && device_is_ready(i2c_dev)) {
			(void)codec_rd(DAC_ADDR, 0xFD, dac_id);
		}
	}
	if (adc_id_out) {
		*adc_id_out = 0;
		if (i2c_dev && device_is_ready(i2c_dev)) {
			(void)codec_rd(IS_ENABLED(CONFIG_ASTER_AUDIO_ADC_ES8311) ? DAC_ADDR : adc_addr,
			               IS_ENABLED(CONFIG_ASTER_AUDIO_ADC_ES8311) ? 0xFD : 0x3D, adc_id_out);
		}
	}
	k_mutex_unlock(&audio_lock);
	return err;
}

int aster_plat_audio_play(const uint8_t *pcm, size_t size)
{
	bool mp3;

	if (!pcm || size == 0) {
		return -EINVAL;
	}
	mp3 = aster_plat_audio_is_mp3(pcm, size);
	if (!mp3 && (size % ASTER_PLAT_AUDIO_FRAME) != 0) {
		return -EINVAL;
	}
	if (sess_busy()) {
		return -EBUSY;
	}
	k_mutex_lock(&audio_lock, K_FOREVER);
	int err = codecs_init();
	if (err) {
		k_mutex_unlock(&audio_lock);
		return err;
	}
	pa_set(true);
	err = mp3 ? play_mp3_blocks(pcm, size) : play_blocks(pcm, size);
	pa_set(false);
	k_mutex_unlock(&audio_lock);
	return err;
}

int aster_plat_audio_record(uint8_t *pcm, size_t size)
{
	if (!pcm || size == 0 || (size % ASTER_PLAT_AUDIO_FRAME) != 0) {
		return -EINVAL;
	}
	if (sess_busy()) {
		return -EBUSY;
	}
	k_mutex_lock(&audio_lock, K_FOREVER);
	int err = codecs_init();
	if (err) {
		k_mutex_unlock(&audio_lock);
		return err;
	}
	err = record_blocks(pcm, size);
	k_mutex_unlock(&audio_lock);
	return err;
}

int aster_plat_audio_beep(uint32_t duration_ms, uint32_t hz)
{
	if (duration_ms == 0 || duration_ms > 8000) {
		return -EINVAL;
	}
	if (hz < 50) {
		hz = 50;
	}
	if (hz > 4000) {
		hz = 4000;
	}
	if (sess_busy()) {
		return -EBUSY;
	}

	k_mutex_lock(&audio_lock, K_FOREVER);
	int err = codecs_init();
	if (err) {
		k_mutex_unlock(&audio_lock);
		return err;
	}
	err = es8311_dac_on();
	if (err) {
		k_mutex_unlock(&audio_lock);
		return err;
	}
	err = i2s_setup(I2S_DIR_TX);
	if (err) {
		k_mutex_unlock(&audio_lock);
		return err;
	}

	pa_set(true);

	int64_t t0 = k_uptime_get();
	const uint32_t frames_total =
		(CONFIG_ASTER_AUDIO_RATE * duration_ms) / 1000u;
	const uint32_t frames_per_block = AUDIO_BLOCK / ASTER_PLAT_AUDIO_FRAME;
	uint32_t step = (uint32_t)(((uint64_t)hz << 16) * 64u / CONFIG_ASTER_AUDIO_RATE);
	uint32_t phase = 0;
	uint32_t frames = 0;
	bool started = false;

	while (frames < frames_total) {
		void *blk = NULL;
		err = k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(AUDIO_TIMEOUT));
		if (err) {
			break;
		}
		int16_t *s = blk;
		uint32_t n = frames_per_block;
		if (frames + n > frames_total) {
			n = frames_total - frames;
		}
		for (uint32_t i = 0; i < n; i++) {
			int16_t v = sine64[(phase >> 16) & 63];
			s[2 * i] = v;
			s[2 * i + 1] = v;
			phase += step;
		}
		if (n < frames_per_block) {
			memset(s + 2 * n, 0, (frames_per_block - n) * ASTER_PLAT_AUDIO_FRAME);
		}
		err = i2s_write(i2s_dev, blk, AUDIO_BLOCK);
		if (err) {
			k_mem_slab_free(&tx_slab, blk);
			break;
		}
		frames += n;
		if (!started) {
			err = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
			if (err) {
				break;
			}
			started = true;
		}
	}
	if (started) {
		(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
		play_wait((size_t)frames * ASTER_PLAT_AUDIO_FRAME, t0);
	} else if (err == 0) {
		err = -EIO;
	}
	i2s_shutdown();
	pa_set(false);
	k_mutex_unlock(&audio_lock);
	return err;
}

int aster_plat_audio_set_volume(int percent)
{
	if (percent < 0 || percent > 100) {
		return -EINVAL;
	}
	g_vol = (uint8_t)percent;
 if(!percent) aster_plat_audio_ui_stop();
	k_mutex_lock(&audio_lock, K_FOREVER);
	int err = codecs_init();
	if (err == 0) {
		err = es8311_apply_vol();
	}
	k_mutex_unlock(&audio_lock);
	return err;
}

int aster_plat_audio_get_volume(void)
{
	return g_vol;
}

int aster_plat_audio_effect_load(const char *path)
{
	struct fs_dirent st;
	struct fs_file_t file;
	uint8_t header[44];
	uint8_t *pcm = NULL;
	int slot = -1;
	int err;
	uint32_t length;

	if (!path || !path[0] || !aster_storage_mounted()) {
		return -EINVAL;
	}
	if (CONFIG_ASTER_AUDIO_BITS != 16 || CONFIG_ASTER_AUDIO_CHANNELS != 2) {
		return -ENOTSUP;
	}
	k_mutex_lock(&effect_mu, K_FOREVER);
	for (int i = 0; i < EFFECT_SLOTS; i++) {
		if (!effects[i].pcm) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		err = -ENOMEM;
		goto done;
	}
	err = fs_stat(path, &st);
	if (err) {
		goto done;
	}
	if (st.size < sizeof(header)) {
		err = -EINVAL;
		goto done;
	}
	fs_file_t_init(&file);
	err = fs_open(&file, path, FS_O_READ);
	if (err) {
		goto done;
	}
	err = aster_fs_read_safe(&file, header, sizeof(header));
	if (err != sizeof(header) || memcmp(header, "RIFF", 4) != 0 ||
	    memcmp(header + 8, "WAVEfmt ", 8) != 0 ||
	    aster_le32(header + 16) != 16 || aster_le16(header + 20) != 1 ||
	    aster_le16(header + 22) != 2 ||
	    aster_le32(header + 24) != CONFIG_ASTER_AUDIO_RATE ||
	    aster_le16(header + 34) != 16 ||
	    memcmp(header + 36, "data", 4) != 0) {
		err = -EINVAL;
		goto close;
	}
	length = aster_le32(header + 40);
	if (!length || length > EFFECT_MAX || (length % ASTER_PLAT_AUDIO_FRAME) ||
	    length > (size_t)st.size - sizeof(header)) {
		err = -EINVAL;
		goto close;
	}
	pcm = aster_psram_alloc(length);
	if (!pcm) {
		err = -ENOMEM;
		goto close;
	}
	err = aster_fs_read_safe(&file, pcm, length);
	if (err != (int)length) {
		err = -EIO;
		aster_psram_free(pcm);
		goto close;
	}
	effects[slot].pcm = pcm;
	effects[slot].size = length;
	err = slot + 1;
close:
	fs_close(&file);
done:
	k_mutex_unlock(&effect_mu);
	return err;
}

/* Never wait for hardware or a closing session on the UI worker. A busy cue
 * is dropped, never queued for late playback. PCM lifetime must be static. */
int aster_plat_audio_ui_play(const int16_t *pcm, size_t frames, unsigned gain,
                            unsigned priority)
{
 if(!pcm || !frames || gain>1000 || CONFIG_ASTER_AUDIO_BITS!=16) return -EINVAL;
 if(!g_vol) return -EAGAIN;
 if(k_mutex_lock(&sess_mu,K_NO_WAIT)) return -EAGAIN;
 if(sess.effect_closing || sess.stop || sess.state==ASTER_AUDIO_PAUSED ||
    k_mutex_lock(&effect_mu,K_NO_WAIT)) {
  k_mutex_unlock(&sess_mu);return -EAGAIN;
 }
 if(ui_voice.pcm && ui_voice.priority>priority) {
  k_mutex_unlock(&effect_mu);k_mutex_unlock(&sess_mu);return -EAGAIN;
 }
 ui_voice.pcm=pcm;ui_voice.frames=frames;ui_voice.off=0;ui_voice.fraction=0;
 ui_voice.gain=gain;ui_voice.priority=priority;
 k_mutex_unlock(&effect_mu);
 if(sess.state==ASTER_AUDIO_IDLE) {
  sess.effect_only=true;sess.size=1;sess.off=0;sess.err=0;
  sess.state=ASTER_AUDIO_PLAYING;
  if (wake_player() != 0) {
   sess_clear_locked();
   k_mutex_unlock(&sess_mu);
   return -ENOMEM;
  }
 }
 k_mutex_unlock(&sess_mu);return 0;
}
void aster_plat_audio_ui_stop(void)
{
 k_mutex_lock(&effect_mu,K_FOREVER);ui_voice.pcm=NULL;
 k_mutex_unlock(&effect_mu);
}

/* On launcher handoff, finish the idle UI session before the APP can use
 * synchronous audio/record. Never stop an APP background playback session. */
void aster_plat_audio_ui_release(void)
{
 aster_plat_audio_ui_stop();
 k_mutex_lock(&sess_mu,K_FOREVER);
 bool idle_ui=sess.effect_only;
 k_mutex_unlock(&sess_mu);
 if(idle_ui) (void)aster_plat_audio_stop();
}

int aster_plat_audio_effect_play(uint32_t handle)
{
	if (!handle || handle > EFFECT_SLOTS) {
		return -EINVAL;
	}
	int64_t deadline = k_uptime_get() + 500;

retry:
	k_mutex_lock(&sess_mu, K_FOREVER);
	if (sess.effect_closing) {
		k_mutex_unlock(&sess_mu);
		if (k_uptime_get() >= deadline) {
			return -EAGAIN;
		}
		k_msleep(1);
		goto retry;
	}
	if (sess.state == ASTER_AUDIO_PAUSED) {
		k_mutex_unlock(&sess_mu);
		return -EAGAIN;
	}
	k_mutex_lock(&effect_mu, K_FOREVER);
	if (!effects[handle - 1].pcm) {
		k_mutex_unlock(&effect_mu);
		k_mutex_unlock(&sess_mu);
		return -EINVAL;
	}
	effects[handle - 1].off = 0;
	effects[handle - 1].fraction = 0;
	effects[handle - 1].playing = true;
	k_mutex_unlock(&effect_mu);
	if (sess.state == ASTER_AUDIO_IDLE) {
		sess.effect_only = true;
		sess.size = 1; /* worker needs a live session, PCM comes from effect slot */
		sess.off = 0;
		sess.err = 0;
		sess.state = ASTER_AUDIO_PLAYING;
		if (wake_player() != 0) {
			k_mutex_lock(&effect_mu, K_FOREVER);
			effects[handle - 1].playing = false;
			k_mutex_unlock(&effect_mu);
			sess_clear_locked();
			k_mutex_unlock(&sess_mu);
			return -ENOMEM;
		}
	}
	k_mutex_unlock(&sess_mu);
	return 0;
}

int aster_plat_audio_effect_unload(uint32_t handle)
{
	if (!handle || handle > EFFECT_SLOTS) {
		return -EINVAL;
	}
	k_mutex_lock(&effect_mu, K_FOREVER);
	effects[handle - 1].playing = false;
	effects[handle - 1].off = 0;
	effects[handle - 1].fraction = 0;
	if (effects[handle - 1].pcm) {
		aster_psram_free(effects[handle - 1].pcm);
		effects[handle - 1].pcm = NULL;
		effects[handle - 1].size = 0;
	}
	k_mutex_unlock(&effect_mu);
	return 0;
}

void aster_plat_audio_effect_reset(void)
{
	k_mutex_lock(&effect_mu, K_FOREVER);
	for (int i = 0; i < EFFECT_SLOTS; i++) {
		effects[i].playing = false;
		effects[i].off = 0;
	effects[i].fraction = 0;
		if (effects[i].pcm) {
			aster_psram_free(effects[i].pcm);
			effects[i].pcm = NULL;
			effects[i].size = 0;
		}
	}
	k_mutex_unlock(&effect_mu);
}

int aster_plat_audio_stop(void)
{
	k_mutex_lock(&sess_mu, K_FOREVER);
	k_mutex_lock(&effect_mu, K_FOREVER);
	for (int i = 0; i < EFFECT_SLOTS; i++) {
		effects[i].playing = false;
		effects[i].off = 0;
	effects[i].fraction = 0;
	}
	k_mutex_unlock(&effect_mu);
	if (sess.state == ASTER_AUDIO_IDLE) {
		k_mutex_unlock(&sess_mu);
		return 0;
	}
	sess.stop = true;
	sess.pause = false;
	k_mutex_unlock(&sess_mu);
	k_sem_give(&play_wake);

	int64_t t0 = k_uptime_get();
	while (sess_busy() && (k_uptime_get() - t0) < 3000) {
		k_msleep(5);
	}
	return sess_busy() ? -ETIMEDOUT : 0;
}

int aster_plat_audio_pause(void)
{
	k_mutex_lock(&sess_mu, K_FOREVER);
	if (sess.state == ASTER_AUDIO_IDLE) {
		k_mutex_unlock(&sess_mu);
		return -EAGAIN;
	}
	sess.pause = true;
	if (sess.state == ASTER_AUDIO_PLAYING) {
		sess.state = ASTER_AUDIO_PAUSED;
	}
	k_mutex_unlock(&sess_mu);
	k_sem_give(&play_wake);
	return 0;
}

int aster_plat_audio_resume(void)
{
	k_mutex_lock(&sess_mu, K_FOREVER);
	if (sess.state == ASTER_AUDIO_IDLE) {
		k_mutex_unlock(&sess_mu);
		return -EAGAIN;
	}
	sess.pause = false;
	if (sess.state == ASTER_AUDIO_PAUSED) {
		sess.state = ASTER_AUDIO_PLAYING;
	}
	k_mutex_unlock(&sess_mu);
	k_sem_give(&play_wake);
	return 0;
}

static void file_abort(void)
{
	if (sess.file_open) {
		fs_close(&sess_file);
		sess.file_open = false;
	}
}

static int path_prepare(const char *path, bool *mp3, size_t *payload)
{
	struct fs_dirent st;
	uint8_t head[64];
	size_t data_off;
	size_t data_len;
	int n;
	int err;

	if (!path || !path[0]) {
		return -EINVAL;
	}
	if (!aster_storage_mounted()) {
		return -ENODEV;
	}
	err = fs_stat(path, &st);
	if (err || st.type != FS_DIR_ENTRY_FILE || st.size < 4) {
		return -EINVAL;
	}

	fs_file_t_init(&sess_file);
	err = fs_open(&sess_file, path, FS_O_READ);
	if (err) {
		return err;
	}
	sess.file_open = true;

	n = aster_fs_read_safe(&sess_file, head, sizeof(head));
	if (n < 4) {
		file_abort();
		return -EIO;
	}

	*mp3 = aster_plat_audio_is_mp3(head, (size_t)n);
	if (*mp3) {
		size_t skip = id3_total(head, (size_t)n);
		uint8_t fh[4];

		if (skip >= (size_t)st.size) {
			file_abort();
			return -EINVAL;
		}
		err = fs_seek(&sess_file, (off_t)skip, FS_SEEK_SET);
		if (err) {
			file_abort();
			return err;
		}
		*payload = (size_t)st.size - skip;
		sess.file_base = skip;
		if (aster_fs_read_safe(&sess_file, fh, 4) == 4) {
			sess.msec_total = mp3_msec_est(fh, *payload);
		} else {
			sess.msec_total = mp3_msec_est(NULL, *payload);
		}
		(void)fs_seek(&sess_file, (off_t)skip, FS_SEEK_SET);
		return 0;
	}

	err = fs_seek(&sess_file, 0, FS_SEEK_SET);
	if (err) {
		file_abort();
		return err;
	}
	err = wav_data_range(&sess_file, (size_t)st.size, &data_off, &data_len);
	if (err) {
		file_abort();
		return err;
	}
	data_len -= data_len % ASTER_PLAT_AUDIO_FRAME;
	if (data_len < ASTER_PLAT_AUDIO_FRAME) {
		file_abort();
		return -EINVAL;
	}
	err = fs_seek(&sess_file, (off_t)data_off, FS_SEEK_SET);
	if (err) {
		file_abort();
		return err;
	}
	*payload = data_len;
	sess.file_base = data_off;
	sess.msec_total = pcm_bytes_to_msec(data_len);
	return 0;
}

static bool path_looks_mp3(const char *path)
{
	const char *dot;

	if (!path) {
		return false;
	}
	dot = strrchr(path, '.');
	if (!dot || !dot[1] || !dot[2] || !dot[3] || dot[4]) {
		return false;
	}
	return (dot[1] == 'm' || dot[1] == 'M') &&
	       (dot[2] == 'p' || dot[2] == 'P') &&
	       (dot[3] == '3');
}

int aster_plat_audio_start_path(const char *path)
{
    k_mutex_lock(&sess_mu, K_FOREVER);
    bool stream_owned = input_stream.handle != 0;
    k_mutex_unlock(&sess_mu);
    if (stream_owned) return -EBUSY;
	bool mp3 = false;
	size_t payload = 0;
	uint8_t *win = NULL;
	int err = aster_plat_audio_stop();

	if (err) {
		return err;
	}
	/* Lyrics before opening the audio file: FatFS one-file-at-a-time
	 * would otherwise invalidate sess_file and the first feed is -EIO. */
	aster_audio_lyrics_load_path(path, path_looks_mp3(path));
	err = path_prepare(path, &mp3, &payload);
	if (err) {
		return err;
	}
	if (mp3) {
		win = aster_psram_alloc(STREAM_WIN);
		if (!win) {
			file_abort();
			return -ENOMEM;
		}
	}

	k_mutex_lock(&sess_mu, K_FOREVER);
	sess.buf = win;
	sess.size = payload;
	sess.off = 0;
	sess.win_len = 0;
	sess.win_off = 0;
	sess.pcm_done = 0;
	sess.pcm_rate = CONFIG_ASTER_AUDIO_RATE;
	sess.seek_pending = false;
	sess.pause = false;
	sess.stop = false;
	sess.mp3 = mp3;
	sess.from_file = true;
	sess.err = 0;
	sess.state = ASTER_AUDIO_PLAYING;
	k_mutex_unlock(&sess_mu);

	while (k_sem_take(&play_wake, K_NO_WAIT) == 0) {
	}
	if (wake_player() != 0) {
		k_mutex_lock(&sess_mu, K_FOREVER);
		file_abort();
		sess_clear_locked();
		k_mutex_unlock(&sess_mu);
		return -ENOMEM;
	}
	return 0;
}

int aster_plat_audio_play_path(const char *path)
{
	int err = aster_plat_audio_start_path(path);

	if (err) {
		return err;
	}
	while (sess_busy()) {
		k_msleep(10);
	}
	k_mutex_lock(&sess_mu, K_FOREVER);
	err = sess.err;
	k_mutex_unlock(&sess_mu);
	return err;
}

int aster_plat_audio_get_state(uint32_t *state, uint32_t *total, uint32_t *done,
			    uint32_t *volume)
{
	k_mutex_lock(&sess_mu, K_FOREVER);
	if (state) {
		*state = sess.state;
	}
	if (total) {
		*total = (uint32_t)sess.size;
	}
	if (done) {
		*done = (uint32_t)sess.off;
	}
	k_mutex_unlock(&sess_mu);
	if (volume) {
		*volume = g_vol;
	}
	return 0;
}

int aster_plat_audio_get_clock(uint32_t *msec_done, uint32_t *msec_total)
{
	k_mutex_lock(&sess_mu, K_FOREVER);
	if (msec_done) {
		*msec_done = sess_msec_now();
	}
	if (msec_total) {
		*msec_total = sess.msec_total;
	}
	k_mutex_unlock(&sess_mu);
	return 0;
}

int aster_plat_audio_seek(uint32_t msec)
{
	int err = 0;

	k_mutex_lock(&sess_mu, K_FOREVER);
    if (sess.from_stream) {
        k_mutex_unlock(&sess_mu);
        return -ENOTSUP;
    }
	if (sess.state == ASTER_AUDIO_IDLE) {
		k_mutex_unlock(&sess_mu);
		return -EAGAIN;
	}
	if (sess.state == ASTER_AUDIO_PAUSED) {
		err = apply_seek_io(msec);
		k_mutex_unlock(&sess_mu);
		return err;
	}
	sess.seek_msec = msec;
	sess.seek_pending = true;
	k_mutex_unlock(&sess_mu);
	k_sem_give(&play_wake);
	return 0;
}

int aster_plat_audio_get_lyrics(char *buf, size_t max)
{
	return aster_audio_lyrics_copy(buf, max);
}

int aster_plat_audio_stream_open(uint32_t codec)
{
    if (codec != ASTER_AUDIO_STREAM_MP3) return -ENOTSUP;
    if (k_mutex_lock(&sess_mu, K_NO_WAIT)) return -EAGAIN;
    if (input_stream.handle || sess.state != ASTER_AUDIO_IDLE) {
        k_mutex_unlock(&sess_mu); return -EBUSY;
    }
    if (input_generation == INT32_MAX) { k_mutex_unlock(&sess_mu); return -ENOSPC; }
    uint8_t *ring = aster_psram_alloc(ASTER_AUDIO_STREAM_CAPACITY);
    uint8_t *window = aster_psram_alloc(STREAM_WIN);
    if (!ring || !window) {
        aster_psram_free(ring);aster_psram_free(window);k_mutex_unlock(&sess_mu);return -ENOMEM;
    }
    memset(&input_stream, 0, sizeof(input_stream));
    input_stream.ring = (struct audio_stream_ring){.data=ring,.cap=ASTER_AUDIO_STREAM_CAPACITY};
    input_stream.handle = ++input_generation;
    input_stream.state = ASTER_AUDIO_STREAM_BUFFERING;
    input_stream.buffering = true;
    sess.buf=window;sess.size=SIZE_MAX;sess.off=0;sess.win_len=0;sess.win_off=0;
    sess.pcm_done=0;sess.pcm_rate=CONFIG_ASTER_AUDIO_RATE;sess.msec_total=0;sess.err=0;sess.pause=false;sess.stop=false;
    sess.mp3=true;sess.from_file=false;sess.from_stream=true;sess.effect_only=false;
    sess.seek_pending=false;sess.state=ASTER_AUDIO_PLAYING;
    uint32_t handle=input_stream.handle;
    k_mutex_unlock(&sess_mu);
    int rc=wake_player();
    if(rc){
        k_mutex_lock(&sess_mu,K_FOREVER);sess.err=rc;sess_clear_locked();input_stream.handle=0;
        k_mutex_unlock(&sess_mu);return rc;
    }
    return (int)handle;
}
int aster_plat_audio_stream_write(uint32_t handle,const void *data,size_t length)
{
    if(!data||!length||length>ASTER_AUDIO_STREAM_WRITE_MAX)return -EINVAL;
    if(k_mutex_lock(&sess_mu,K_NO_WAIT))return -EAGAIN;
    int rc;
    if(!handle||handle!=input_stream.handle)rc=-EBADF;
    else if(input_stream.error)rc=input_stream.error;
    else if(input_stream.eof||input_stream.state>=ASTER_AUDIO_STREAM_DONE)rc=-EPIPE;
    else {
        rc=(int)audio_ring_write(&input_stream.ring,data,length);
        input_stream.received+=(unsigned)rc;
        if(!rc)rc=-EAGAIN;
    }
    k_mutex_unlock(&sess_mu);k_sem_give(&play_wake);return rc;
}
int aster_plat_audio_stream_finish(uint32_t handle)
{
    if(k_mutex_lock(&sess_mu,K_NO_WAIT))return -EAGAIN;
    int rc=(!handle||handle!=input_stream.handle)?-EBADF:0;
    if(!rc)input_stream.eof=true;
    k_mutex_unlock(&sess_mu);k_sem_give(&play_wake);return rc;
}
int aster_plat_audio_stream_status(uint32_t handle,aster_audio_stream_status_t *out)
{
    if(!out)return -EINVAL;
    if(k_mutex_lock(&sess_mu,K_NO_WAIT))return -EAGAIN;
    if(!handle||handle!=input_stream.handle){k_mutex_unlock(&sess_mu);return -EBADF;}
    uint32_t state=input_stream.state;
    if(sess.from_stream)state=sess.pause?ASTER_AUDIO_STREAM_PAUSED:
        input_stream.buffering?ASTER_AUDIO_STREAM_BUFFERING:ASTER_AUDIO_STREAM_PLAYING;
    *out=(aster_audio_stream_status_t){.size=sizeof(*out),.version=1,.state=state,
        .error=input_stream.error,.queued=input_stream.ring.used,.capacity=ASTER_AUDIO_STREAM_CAPACITY,
        .underruns=input_stream.underruns,.flags=input_stream.eof?ASTER_AUDIO_STREAM_F_EOF:0,
        .received=input_stream.received,.consumed=input_stream.consumed,.played_ms=input_stream.played_ms};
    k_mutex_unlock(&sess_mu);return 0;
}
int aster_plat_audio_stream_close(uint32_t handle)
{
    if(k_mutex_lock(&sess_mu,K_NO_WAIT))return -EAGAIN;
    if(!handle||handle!=input_stream.handle){k_mutex_unlock(&sess_mu);return -EBADF;}
    input_stream.handle=0;
    if(sess.from_stream){sess.stop=true;sess.pause=false;}
    k_mutex_unlock(&sess_mu);k_sem_give(&play_wake);return 0;
}
