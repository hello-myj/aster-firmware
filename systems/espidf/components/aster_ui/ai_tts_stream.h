/* SPDX-License-Identifier: MIT */
#pragma once
#include "deepseek.h"
#include "ai_reply_audio.h"

int ai_tts_stream(deepseek_turn_t *api, struct ai_reply_sink *sink, unsigned prebuffer_blocks,
                  int (*produce)(void *), void *context, void (*on_speaking)(void));
