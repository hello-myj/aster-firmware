/* SPDX-License-Identifier: MIT */
#include "aster_idf_usb_sdmmc.h"
#include "aster_power_ui.h"
#include "aster_os.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tusb.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>

enum { DMA_BYTES = 8192, SCSI_SYNC_CACHE = 0x35, SCSI_SYNC_CACHE_16 = 0x91 };

static const char *TAG = "usb_storage";
static sdmmc_card_t card;
static uint8_t *dma;
static StaticSemaphore_t lock_buffer;
static SemaphoreHandle_t io_lock;
static atomic_bool io_enabled, host_seen, host_detached, ejected;

static bool medium_ready(uint8_t lun)
{
    if (lun == 0 && atomic_load(&io_enabled) && !atomic_load(&ejected))
        return true;
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0);
    return false;
}

bool __wrap_tud_msc_test_unit_ready_cb(uint8_t lun)
{
    return medium_ready(lun);
}

/* TinyUSB runs these callbacks on its own task. Finish the SD I/O before
 * acknowledging a command, so the host sees actual write errors. */
static int32_t transfer(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *data, uint32_t size,
                        bool write)
{
    xSemaphoreTake(io_lock, portMAX_DELAY);
    if (!medium_ready(lun)) {
        xSemaphoreGive(io_lock);
        return -1;
    }
    size_t sector_size = card.csd.sector_size;
    uint64_t position = (uint64_t)lba * sector_size + offset;
    uint64_t capacity = (uint64_t)card.csd.capacity * sector_size;
    if (position > capacity || size > capacity - position) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0);
        xSemaphoreGive(io_lock);
        return -1;
    }
    size_t remaining = size;
    esp_err_t result = ESP_OK;
    while (remaining && result == ESP_OK) {
        size_t sector = position / sector_size;
        size_t skip = position % sector_size;
        size_t sectors = !skip ? remaining / sector_size : 0;
        if (sectors > DMA_BYTES / sector_size)
            sectors = DMA_BYTES / sector_size;
        size_t length;
        if (sectors) {
            length = sectors * sector_size;
            if (write) {
                memcpy(dma, data, length);
                result = sdmmc_write_sectors(&card, dma, sector, sectors);
            } else {
                result = sdmmc_read_sectors(&card, dma, sector, sectors);
                if (result == ESP_OK)
                    memcpy(data, dma, length);
            }
        } else {
            length = sector_size - skip;
            if (length > remaining)
                length = remaining;
            result = sdmmc_read_sectors(&card, dma, sector, 1);
            if (result == ESP_OK && write) {
                memcpy(dma + skip, data, length);
                result = sdmmc_write_sectors(&card, dma, sector, 1);
            } else if (result == ESP_OK) {
                memcpy(data, dma + skip, length);
            }
        }
        position += length;
        remaining -= length;
        data += length;
    }
    xSemaphoreGive(io_lock);
    if (result == ESP_OK)
        return size;
    ESP_LOGW(TAG, "%s failed at LBA %lu: %s", write ? "write" : "read", (unsigned long)lba,
             esp_err_to_name(result));
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, write ? 0x0c : 0x11, 0);
    return -1;
}

int32_t __wrap_tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer,
                                 uint32_t size)
{
    return transfer(lun, lba, offset, buffer, size, false);
}

int32_t __wrap_tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer,
                                  uint32_t size)
{
    return transfer(lun, lba, offset, buffer, size, true);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t condition, bool start, bool eject)
{
    (void)condition;
    if (lun != 0)
        return false;
    if (eject && !start) {
        atomic_store(&ejected, true);
        return true;
    }
    return medium_ready(lun);
}

int32_t __real_tud_msc_scsi_cb(uint8_t lun, const uint8_t command[16], void *buffer,
                               uint16_t length);

int32_t __wrap_tud_msc_scsi_cb(uint8_t lun, const uint8_t command[16], void *buffer,
                               uint16_t length)
{
    if (command[0] == SCSI_SYNC_CACHE || command[0] == SCSI_SYNC_CACHE_16)
        return medium_ready(lun) ? 0 : -1; /* Each write already completed. */
    return __real_tud_msc_scsi_cb(lun, command, buffer, length);
}

static void usb_event(tinyusb_event_t *event, void *context)
{
    (void)context;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        atomic_store(&host_detached, false);
        atomic_store(&host_seen, true);
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        atomic_store(&host_detached, true);
    }
}

static bool unplugged(const aster_idf_usb_sdmmc_ops_t *ops)
{
    return ops->unplugged ? ops->unplugged() : atomic_load(&host_detached);
}

int aster_idf_usb_sdmmc_run(const aster_idf_usb_sdmmc_ops_t *ops)
{
    if (!io_lock)
        io_lock = xSemaphoreCreateMutexStatic(&lock_buffer);
    dma = heap_caps_malloc(DMA_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!dma)
        return -ENOMEM;

    bool raw_open = false, driver_open = false;
    tinyusb_msc_storage_handle_t storage = NULL;
    esp_err_t result = ops->open(&card);
    if (result != ESP_OK)
        goto finish;
    raw_open = true;
    if (card.csd.sector_size == 0 || card.csd.sector_size > DMA_BYTES) {
        result = ESP_ERR_NOT_SUPPORTED;
        goto finish;
    }
    const tinyusb_msc_driver_config_t msc = {.user_flags.auto_mount_off = 1};
    result = tinyusb_msc_install_driver(&msc);
    if (result != ESP_OK)
        goto finish;
    driver_open = true;
    const tinyusb_msc_storage_config_t medium = {
        .medium.card = &card,
        .fat_fs = {.do_not_format = true},
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    result = tinyusb_msc_new_storage_sdmmc(&medium, &storage);
    if (result != ESP_OK)
        goto finish;

    atomic_store(&host_seen, false);
    atomic_store(&host_detached, false);
    atomic_store(&ejected, false);
    atomic_store(&io_enabled, true);
    const tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG(usb_event);
    result = tinyusb_driver_install(&usb);
    if (result != ESP_OK)
        goto finish;
    ESP_LOGI(TAG, "SD exported over USB (%lu sectors)", (unsigned long)card.csd.capacity);

    while (!atomic_load(&ejected)) {
        bool connected = atomic_load(&host_seen);
        if (aster_power_ui_usb_tick(connected, ops->connection_hint) && !atomic_load(&host_seen))
            break;
        if (connected && unplugged(ops)) {
            aster_os_sleep_ms(100);
            if (unplugged(ops))
                break;
        }
        aster_os_sleep_ms(30);
    }

    /* Block incoming I/O, then wait for any transfer that was already
     * running. Give a host eject command time to receive its final reply. */
    atomic_store(&io_enabled, false);
    xSemaphoreTake(io_lock, portMAX_DELAY);
    xSemaphoreGive(io_lock);
    if (atomic_load(&ejected))
        aster_os_sleep_ms(250);
    result = tinyusb_driver_uninstall();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "USB teardown failed: %s; restarting to release it", esp_err_to_name(result));
        esp_restart();
    }

finish:
    atomic_store(&io_enabled, false);
    if (storage)
        tinyusb_msc_delete_storage(storage);
    if (driver_open)
        tinyusb_msc_uninstall_driver();
    if (raw_open) {
        esp_err_t close_result = ops->close();
        if (result == ESP_OK)
            result = close_result;
    }
    heap_caps_free(dma);
    dma = NULL;
    ESP_LOGI(TAG, "USB session finished: %s", esp_err_to_name(result));
    return result == ESP_OK ? 0 : -EIO;
}
