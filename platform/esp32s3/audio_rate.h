/* SPDX-License-Identifier: MIT. Native-PCM cue conversion, no allocations. */
#ifndef ASTER_AUDIO_RATE_H
#define ASTER_AUDIO_RATE_H
#include <stdint.h>
#include <stddef.h>

/* Fraction is a rational source-frame position, denominator output_rate.
 * Linear interpolation is for short 16kHz UI/effect cues. MP3 music itself
 * bypasses resampling and is sent at its decoded sample rate. */
static int16_t audio_rate_sample(const int16_t *pcm, size_t frames,
                                unsigned channels, unsigned channel,
                                size_t frame, uint32_t fraction,
                                uint32_t output_rate)
{
    if (frame >= frames) return 0;
    int32_t a = pcm[frame * channels + channel];
    int32_t b = pcm[(frame + 1 < frames ? frame + 1 : frame) * channels + channel];
    return (int16_t)(a + (int64_t)(b - a) * fraction / output_rate);
}
static void audio_rate_advance(size_t *frame, uint32_t *fraction,
                               uint32_t input_rate, uint32_t output_rate)
{
    uint32_t n = *fraction + input_rate;
    *frame += n / output_rate;
    *fraction = n % output_rate;
}
static uint32_t audio_mp3_rate(uint32_t rate)
{
    switch (rate) {
    case 8000: case 11025: case 12000: case 16000: case 22050:
    case 24000: case 32000: case 44100: case 48000: return rate;
    default: return 0;
    }
}
static size_t audio_rate_block(uint32_t rate)
{
    return rate > 32000 ? 1536u : rate > 16000 ? 1024u : 512u;
}
#endif
