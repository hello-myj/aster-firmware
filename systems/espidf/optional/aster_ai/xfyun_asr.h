/* SPDX-License-Identifier: MIT */
#pragma once
#include "ai_credentials.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct xfyun_asr;
int xfyun_asr_start(struct xfyun_asr **out, const struct ai_speech_keys *keys);
int xfyun_asr_feed(struct xfyun_asr *asr, const int16_t *mono, size_t samples);
void xfyun_asr_seal(struct xfyun_asr *asr, size_t samples);
int xfyun_asr_transcribe(struct xfyun_asr **asr, const struct ai_speech_keys *keys, int stream_error,
                        const int16_t *mono, size_t samples, char *question, size_t capacity,
                        bool (*cancelled)(void));
void xfyun_asr_close(struct xfyun_asr *asr);
void xfyun_asr_close_async(struct xfyun_asr *asr);
