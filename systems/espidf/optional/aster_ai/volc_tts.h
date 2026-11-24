/* SPDX-License-Identifier: MIT */
#pragma once
#include "ai_credentials.h"
#include "deepseek.h"

struct volc_tts;
int volc_tts_open(struct volc_tts **voice, deepseek_turn_t *api, const struct ai_speech_keys *keys);
int volc_tts_delta(struct volc_tts *voice, const char *text, size_t length);
int volc_tts_finish(struct volc_tts *voice, const char *text);
void volc_tts_close(struct volc_tts *voice);
