/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

typedef struct {
    const char *connection_hint;
    esp_err_t (*open)(sdmmc_card_t *card);
    esp_err_t (*close)(void);
    /* Optional board-level cable detection when VBUS is available. */
    bool (*unplugged)(void);
} aster_idf_usb_sdmmc_ops_t;

/* Called from the power session with the SD filesystem unmounted. */
int aster_idf_usb_sdmmc_run(const aster_idf_usb_sdmmc_ops_t *ops);
