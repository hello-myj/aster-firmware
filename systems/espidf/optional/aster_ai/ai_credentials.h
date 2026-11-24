/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define AI_KEY_CAP 160
struct ai_speech_keys {
    char app_id[24], api_key[65], api_secret[65], volc_key[96], speaker[80];
};
int ai_speech_keys_clear(void);
int ai_speech_keys_read(struct ai_speech_keys *keys);
int ai_speech_keys_write(const struct ai_speech_keys *keys);
bool ai_key_present(void);
int ai_key_read(char key[AI_KEY_CAP]);
int ai_key_write(const char *key, size_t length);
void ai_secret_clear(void *bytes, size_t size);
