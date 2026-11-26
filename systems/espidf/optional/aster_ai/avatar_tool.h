/* SPDX-License-Identifier: MIT */
#pragma once
#include "avatar_asset.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>

struct ai_avatar_call {
    char arguments[2560];
    size_t used;
    bool seen, named, finished, invalid;
};

bool ai_avatar_tool_add(cJSON *request, const struct ai_avatar_catalog *catalog);
void ai_avatar_tool_collect(struct ai_avatar_call *call, const cJSON *choice, const cJSON *delta);
bool ai_avatar_tool_resolve(const struct ai_avatar_call *call, const struct ai_avatar_catalog *catalog,
                            struct ai_avatar_cue *cue, char *speech, size_t capacity);
