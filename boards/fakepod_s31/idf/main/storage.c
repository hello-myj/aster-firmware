/* SPDX-License-Identifier: MIT */
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "aster_idf_runtime.h"
#include "board_pins.h"
#include "aster_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include <errno.h>
#if CONFIG_ASTER_FAKEPOD_USB_STORAGE
#include "usb_storage.h"
#include "aster_idf_usb_sdmmc.h"
#include "aster_power.h"
#endif
static sdmmc_card_t *mounted_card;
void fakepod_storage_start(void);

static int power_suspend(void)
{
    if (!mounted_card)
        return 0;
    esp_err_t r = esp_vfs_fat_sdcard_unmount("/SD", mounted_card);
    if (!r)
        mounted_card = NULL;
    return r ? -EIO : 0;
}

static int power_resume(void)
{
    fakepod_storage_start();
    return mounted_card ? 0 : -EIO;
}

static int format_and_reboot(void)
{
    if (!mounted_card)
        return -ENODEV;
    ESP_LOGW("aster", "SD FORMAT begin: all card contents will be erased");
    esp_err_t result = esp_vfs_fat_sdcard_format("/SD", mounted_card);
    if (result != ESP_OK) {
        ESP_LOGE("aster", "SD FORMAT failed: %s", esp_err_to_name(result));
        return -EIO;
    }
    ESP_LOGW("aster", "SD FORMAT complete; rebooting into empty-card setup");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static sdmmc_slot_config_t board_sd_slot(void)
{
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = FP_SD_CLOCK;
    slot.cmd = FP_SD_CMD;
    slot.d0 = FP_SD_D0;
    slot.d1 = FP_SD_D1;
    slot.d2 = FP_SD_D2;
    slot.d3 = FP_SD_D3;
    slot.cd = GPIO_NUM_NC;
    slot.wp = GPIO_NUM_NC;
    return slot;
}

#if CONFIG_ASTER_FAKEPOD_USB_STORAGE
static esp_err_t open_raw(sdmmc_card_t *card)
{
    if (mounted_card || aster_storage_mounted())
        return ESP_ERR_INVALID_STATE;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    esp_err_t result = sdmmc_host_init();
    if (result != ESP_OK)
        return result;
    sdmmc_slot_config_t slot = board_sd_slot();
    result = sdmmc_host_init_slot(host.slot, &slot);
    if (result == ESP_OK)
        result = sdmmc_card_init(&host, card);
    if (result != ESP_OK)
        sdmmc_host_deinit();
    return result;
}

static bool usb_unplugged(void)
{
    aster_power_battery_t battery;
    return aster_power_battery(&battery) == 0 && !(battery.flags & ASTER_POWER_EXTERNAL_PRESENT);
}

int fakepod_usb_storage(void *context)
{
    (void)context;
    static const aster_idf_usb_sdmmc_ops_t ops = {
        .connection_hint = "请连接电脑。若只识别到串口，请翻转设备端插头。",
        .open = open_raw, .close = sdmmc_host_deinit, .unplugged = usb_unplugged,
    };
    return aster_idf_usb_sdmmc_run(&ops);
}
#endif

void fakepod_storage_start(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = board_sd_slot();
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
    mounted_card = card;
    aster_storage_set_power_handlers(power_suspend, power_resume);
    aster_storage_set_format_handler(format_and_reboot);
    int initialized = aster_storage_init();
    if (initialized)
        ESP_LOGE("aster", "Storage directories failed: %d", initialized);
}
