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
#if CONFIG_ASTER_OSPTEK_USB_STORAGE
#include "aster_idf_usb_sdmmc.h"
#include "aster_idf_apps.h"
#endif
#include "storage.h"

static sdmmc_card_t *mounted_card;

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
    osptek_storage_start();
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

static sdmmc_host_t board_sd_host(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = ASTER_STORAGE_SLOT;
    host.init = aster_storage_sdmmc_init;
    host.flags |= SDMMC_HOST_FLAG_DEINIT_ARG;
    host.deinit_p = aster_storage_sdmmc_deinit;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    return host;
}

static sdmmc_slot_config_t board_sd_slot(void)
{
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = 43;
    slot.cmd = 44;
    slot.d0 = 39;
    slot.d1 = 40;
    slot.d2 = 41;
    slot.d3 = 42;
    slot.cd = GPIO_NUM_NC; /* Detect GPIO45 is documented, initialization probes the card. */
    slot.wp = GPIO_NUM_NC;
    return slot;
}

#if CONFIG_ASTER_OSPTEK_USB_STORAGE
static esp_err_t close_raw(void)
{
    /* Slot 1 belongs to the C5 even when the local filesystem is offline. */
    return aster_storage_sdmmc_deinit(ASTER_STORAGE_SLOT);
}

static esp_err_t open_raw(sdmmc_card_t *card)
{
    if (mounted_card || aster_storage_mounted())
        return ESP_ERR_INVALID_STATE;
    sdmmc_host_t host = board_sd_host();
    sdmmc_slot_config_t slot = board_sd_slot();
    esp_err_t result = host.init();
    if (result != ESP_OK)
        return result;
    result = sdmmc_host_init_slot(host.slot, &slot);
    if (result == ESP_OK)
        result = sdmmc_card_init(&host, card);
    if (result != ESP_OK)
        close_raw();
    return result;
}

static int usb_storage(void *context)
{
    (void)context;
    static const aster_idf_usb_sdmmc_ops_t ops = {
        .connection_hint = "请用数据线连接电脑和板上的原生 USB Type-C 接口。",
        .open = open_raw,
        .close = close_raw,
    };
    return aster_idf_usb_sdmmc_run(&ops);
}

static int power_caps(void *context, aster_power_caps_t *caps)
{
    int result = aster_idf_power_base.caps(context, caps);
    if (!result)
        caps->modes |= ASTER_POWER_USB_STORAGE;
    return result;
}

void osptek_usb_storage_register(void)
{
    static aster_power_ops_t ops;
    const aster_power_ops_t overrides = {
        .size = sizeof(overrides), .version = ASTER_POWER_API_VERSION,
        .name = "osptek-p4-sd", .caps = power_caps, .usb_storage = usb_storage,
    };
    int result = aster_power_derive(&ops, &aster_idf_power_base, &overrides);
    if (!result)
        result = aster_power_bind(&ops, NULL);
    if (result)
        ESP_LOGE("aster", "USB storage registration failed: %d", result);
}
#endif

void osptek_storage_start(void)
{
    sdmmc_host_t host = board_sd_host();
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
