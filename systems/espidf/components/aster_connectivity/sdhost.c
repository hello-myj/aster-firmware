/* SPDX-License-Identifier: MIT */
/* Pinned to IDF 6.0 legacy controller API. Startup mounts storage before
 * launching the Hosted worker; slot registration is serialized at boot.
 * Subsequent C5 release never tears down the card's slot or controller. */
#include "aster_idf_sdhost.h"
#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
static SemaphoreHandle_t lock;
static bool ready, owner[2];
static esp_err_t acquire(int slot)
{
    /* First call is on app_main, before any Hosted worker exists. */
    if (!lock) { lock=xSemaphoreCreateMutex();if (!lock)return ESP_ERR_NO_MEM; }
    xSemaphoreTake(lock,portMAX_DELAY);
    esp_err_t result=ESP_OK;
    if (!ready) {result=sdmmc_host_init();if(result==ESP_OK)ready=true;}
    if (result==ESP_OK)owner[slot]=true;
    xSemaphoreGive(lock);return result;
}
static esp_err_t release(int slot)
{
    if (!lock)return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock,portMAX_DELAY);
    if (!owner[slot]) {xSemaphoreGive(lock);return ESP_ERR_INVALID_STATE;}
    esp_err_t result=sdmmc_host_deinit_slot(slot);
    owner[slot]=false;
    if (!owner[0] && !owner[1]) {
        /* A failed slot init has no slot to remove, but still owns a host. */
        if(result!=ESP_OK)result=sdmmc_host_deinit();
        ready=false;
    }
    xSemaphoreGive(lock);return result;
}
esp_err_t aster_storage_sdmmc_init(void) { return acquire(0); }
esp_err_t aster_storage_sdmmc_deinit(int slot)
{ return slot==0 ? release(0) : ESP_ERR_INVALID_ARG; }
esp_err_t aster_hosted_sdmmc_init(void) { return acquire(1); }
esp_err_t aster_hosted_sdmmc_deinit(void) { return release(1); }
