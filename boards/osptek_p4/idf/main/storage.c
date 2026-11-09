/* SPDX-License-Identifier: MIT */
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "aster_idf_runtime.h"
#include "aster_idf_sdhost.h"
#include "aster_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include <errno.h>
static sdmmc_card_t *mounted_card;
static int format_and_reboot(void)
{
    if(!mounted_card)return -ENODEV;
    ESP_LOGW("aster","SD FORMAT begin: all card contents will be erased");
    esp_err_t result=esp_vfs_fat_sdcard_format("/SD",mounted_card);
    if(result!=ESP_OK){ESP_LOGE("aster","SD FORMAT failed: %s",esp_err_to_name(result));return -EIO;}
    ESP_LOGW("aster","SD FORMAT complete; rebooting into empty-card setup");
    vTaskDelay(pdMS_TO_TICKS(300));esp_restart();return 0;
}

void osptek_storage_start(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.init=aster_storage_sdmmc_init;
    host.flags |= SDMMC_HOST_FLAG_DEINIT_ARG;
    host.deinit_p=aster_storage_sdmmc_deinit;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = 43; slot.cmd = 44;
    slot.d0 = 39; slot.d1 = 40; slot.d2 = 41; slot.d3 = 42;
    slot.cd = GPIO_NUM_NC; /* Detect GPIO45 is documented, initialization probes the card. */
    slot.wp = GPIO_NUM_NC;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 32,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/SD", &host, &slot, &mount, &card);
    if (ret != ESP_OK) {
        ESP_LOGW("aster", "SD unavailable (never auto-format): %s", esp_err_to_name(ret));
        return;
    }
    sdmmc_card_print_info(stdout, card);
    mounted_card=card;
    aster_storage_set_format_handler(format_and_reboot);
    int initialized=aster_storage_init();
    if(initialized)ESP_LOGE("aster","Storage directories failed: %d",initialized);
}
