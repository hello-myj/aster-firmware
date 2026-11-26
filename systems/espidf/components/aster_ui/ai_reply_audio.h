/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { AI_PCM_BLOCK_BYTES = 16 * 1024 };
struct ai_pcm_block {
    struct ai_pcm_block *next;
    size_t used;
    uint8_t pcm[AI_PCM_BLOCK_BYTES];
};
struct ai_reply_audio {
    struct ai_pcm_block *first, *last;
    size_t bytes;
    int (*refill)(void *ctx, struct ai_pcm_block **block);
    void *context;
};

struct ai_reply_sink {
    uint32_t endpoint, max_frames;
    int32_t *stereo;
};

int ai_reply_audio_append(void *context, const uint8_t *data, size_t bytes);
int ai_reply_sink_open(struct ai_reply_sink *sink);
int ai_reply_sink_close(struct ai_reply_sink *sink);
int ai_reply_audio_play(struct ai_reply_audio *audio, struct ai_reply_sink *sink, bool (*cancelled)(void));
void ai_reply_audio_clear(struct ai_reply_audio *audio);
bool ai_reply_audio_voicing(void);
unsigned ai_reply_audio_level(void);
