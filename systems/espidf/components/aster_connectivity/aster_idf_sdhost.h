/* SPDX-License-Identifier: MIT */
#pragma once
#include "esp_err.h"
esp_err_t aster_storage_sdmmc_init(void);
esp_err_t aster_storage_sdmmc_deinit(int slot);
