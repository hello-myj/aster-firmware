/* SPDX-License-Identifier: MIT */
#pragma once
#include "esp_log.h"
#define LOG_LEVEL_INF ESP_LOG_INFO
#define LOG_MODULE_REGISTER(name,level) static const char *aster_log_tag=#name
#define LOG_INF(...) ESP_LOGI(aster_log_tag, __VA_ARGS__)
#define LOG_WRN(...) ESP_LOGW(aster_log_tag, __VA_ARGS__)
#define LOG_ERR(...) ESP_LOGE(aster_log_tag, __VA_ARGS__)
#define LOG_DBG(...) ESP_LOGD(aster_log_tag, __VA_ARGS__)
