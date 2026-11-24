/* SPDX-License-Identifier: MIT */
#pragma once

enum ai_voice_status { AI_IDLE, AI_LOADING, AI_LISTENING, AI_HEARD, AI_BUSY, AI_FAILED };

enum ai_voice_status ai_voice_status(void);
void ai_voice_start(void);
void ai_voice_stop(void);
