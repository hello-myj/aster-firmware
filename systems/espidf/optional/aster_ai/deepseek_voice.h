/* SPDX-License-Identifier: MIT */
#pragma once
#include "deepseek.h"

struct deepseek_voice;
struct deepseek_voice *deepseek_voice_begin(deepseek_turn_t *api);
void deepseek_voice_delta(struct deepseek_voice *v, const char *text, size_t length);
int deepseek_voice_finish(struct deepseek_voice *v, deepseek_turn_t *api, const char *answer);
void deepseek_voice_close(struct deepseek_voice *v);
