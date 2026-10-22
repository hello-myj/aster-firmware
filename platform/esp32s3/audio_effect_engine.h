/* SPDX-License-Identifier: MIT */
#ifndef ASTER_AUDIO_EFFECT_ENGINE_H
#define ASTER_AUDIO_EFFECT_ENGINE_H
#include <aster/audio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
 const int16_t *pcm;
 size_t size;
 uint32_t frames, rate, channels;
 uint64_t phase, cached_step;
 uint32_t cached_rate, cached_pitch;
 uint32_t step_remainder, step_error, ramp_rate;
 uint32_t serial;
 aster_audio_effect_play_desc_t desc;
 int32_t pitch, gain, pitch_from, gain_from;
 uint32_t pitch_target, gain_target, ramp_ms, ramp_count, ramp_total;
 bool playing, stopping, loading;
} audio_effect_voice_t;

int audio_effect_begin(audio_effect_voice_t *v,const aster_audio_effect_play_desc_t *d);
int audio_effect_params(audio_effect_voice_t *v,const aster_audio_effect_params_t *p);
int audio_effect_stop(audio_effect_voice_t *v,uint32_t ms);
/* No allocation, locks, IO or clocks. Sum into caller-owned 32-bit channels. */
void audio_effect_sample(audio_effect_voice_t *v,uint32_t output_rate,int32_t *l,int32_t *r);
#endif
