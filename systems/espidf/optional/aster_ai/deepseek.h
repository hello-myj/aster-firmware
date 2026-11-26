/* SPDX-License-Identifier: MIT */
#pragma once
#include "avatar_asset.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEEPSEEK_MODEL "deepseek-flash"
#define AI_QUESTION_CAP 1024
#define AI_REPLY_CAP 2048
struct deepseek_exchange {
    char question[AI_QUESTION_CAP], answer[AI_REPLY_CAP];
};

typedef struct {
    const char *key;
    bool (*cancelled)(void);
    int (*receive_pcm)(void *output, const uint8_t *data, size_t length);
    int (*end_pcm)(void *output);
    int (*flush_pcm)(void *output);
    bool single_utterance; /* Compare a full reply with first-clause synthesis in board diagnostics. */
    void *output;
    int http_status;
    void *connection;
    unsigned connections;
    const struct ai_avatar_catalog *avatar;
    struct ai_avatar_cue *avatar_cue;
} deepseek_turn_t;

int deepseek_asr(deepseek_turn_t *turn, const int16_t *mono, size_t samples,
                 char text[AI_QUESTION_CAP]);
int deepseek_chat(deepseek_turn_t *turn, const char *question, const struct deepseek_exchange *history,
                  size_t count, char answer[AI_REPLY_CAP], int (*on_delta)(void *, size_t),
                  void *context);
int deepseek_tts(deepseek_turn_t *turn, const char *answer);
void deepseek_close(deepseek_turn_t *turn);
int deepseek_warm_chat(deepseek_turn_t *turn);
