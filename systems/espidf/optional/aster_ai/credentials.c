/* SPDX-License-Identifier: MIT */
#include "ai_credentials.h"
#include "nvs.h"
#include <errno.h>
#include <ctype.h>
#include <string.h>

void ai_secret_clear(void *bytes, size_t size)
{
    volatile unsigned char *p = bytes;
    while (size--)
        *p++ = 0;
}

static int read_key(char *key)
{
    nvs_handle_t nvs;
    if (nvs_open("aster_ai", NVS_READONLY, &nvs) != ESP_OK)
        return -ENOENT;
    size_t n = AI_KEY_CAP;
    esp_err_t err = nvs_get_str(nvs, "deepseek_key", key, &n);
    nvs_close(nvs);
    return err == ESP_OK && n > 1 && n <= AI_KEY_CAP ? 0 : -ENOENT;
}

bool ai_key_present(void)
{
    return read_key(NULL) == 0;
}

int ai_key_read(char key[AI_KEY_CAP])
{
    return read_key(key);
}

int ai_key_write(const char *key, size_t length)
{
    if (length >= AI_KEY_CAP || (length && (length < 12 || !key || strnlen(key, AI_KEY_CAP) != length)))
        return -EINVAL;
    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)key[i] < 33 || (unsigned char)key[i] > 126)
            return -EINVAL;
    }
    nvs_handle_t nvs;
    if (nvs_open("aster_ai", NVS_READWRITE, &nvs) != ESP_OK)
        return -EIO;
    esp_err_t err = length ? nvs_set_str(nvs, "deepseek_key", key) : nvs_erase_key(nvs, "deepseek_key");
    if (!length && err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK ? 0 : -EIO;
}

static bool valid_field(const char *s, size_t cap)
{
    size_t len = strnlen(s, cap);
    if (!len || len == cap)
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = s[i];
        if (!isalnum(ch) && ch != '-' && ch != '_')
            return false;
    }
    return true;
}

static bool valid_speech_keys(const struct ai_speech_keys *keys)
{
    return valid_field(keys->app_id, sizeof(keys->app_id)) &&
           valid_field(keys->api_key, sizeof(keys->api_key)) &&
           valid_field(keys->api_secret, sizeof(keys->api_secret)) &&
           valid_field(keys->volc_key, sizeof(keys->volc_key)) &&
           valid_field(keys->speaker, sizeof(keys->speaker));
}

int ai_speech_keys_read(struct ai_speech_keys *keys)
{
    nvs_handle_t nvs;
    if (nvs_open("aster_ai", NVS_READONLY, &nvs) != ESP_OK)
        return -ENOENT;
    size_t length = sizeof(*keys);
    esp_err_t err = nvs_get_blob(nvs, "speech_v1", keys, &length);
    nvs_close(nvs);
    return err == ESP_OK && length == sizeof(*keys) && valid_speech_keys(keys) ? 0 : -ENOENT;
}

int ai_speech_keys_write(const struct ai_speech_keys *keys)
{
    if (!valid_speech_keys(keys))
        return -EINVAL;
    nvs_handle_t nvs;
    if (nvs_open("aster_ai", NVS_READWRITE, &nvs) != ESP_OK)
        return -EIO;
    esp_err_t err = nvs_set_blob(nvs, "speech_v1", keys, sizeof(*keys));
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK ? 0 : -EIO;
}

int ai_speech_keys_clear(void)
{
    nvs_handle_t nvs;
    if (nvs_open("aster_ai", NVS_READWRITE, &nvs) != ESP_OK)
        return -EIO;
    esp_err_t err = nvs_erase_key(nvs, "speech_v1");
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK ? 0 : -EIO;
}
