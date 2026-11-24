/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>

enum { AI_MIC_RATE = 16000, AI_PREROLL = AI_MIC_RATE / 2, AI_WAIT_LIMIT = AI_MIC_RATE * 10,
       AI_TURN_LIMIT = AI_MIC_RATE * 15 };

struct ai_endpoint {
    size_t captured;
    bool heard;
};

/* VADNet already debounces onset and silence. Keep half a second before its
 * first positive decision so that recognition gets the start of the word. */
static inline size_t ai_endpoint_leading_skip(size_t buffered)
{
    return buffered > AI_PREROLL ? buffered - AI_PREROLL : 0;
}

static inline bool ai_endpoint_finished(const struct ai_endpoint *gate, bool speech)
{
    if (!gate->heard)
        return gate->captured >= AI_WAIT_LIMIT;
    return !speech && gate->captured >= 2 * AI_MIC_RATE;
}
