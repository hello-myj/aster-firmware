/*
 * SPDX-FileCopyrightText: Copyright 2026 OSPTEK
 * Modifications: Aster hardware diagnostic, configurable pins and RGB565 color bars.
 * Source: supplied display-tft-3.95inch-480x480-main.zip,
 * examples/esp32p4-st7102-mipi-bringup/main/main.c
 * SPDX-License-Identifier: CC-BY-4.0
 *
 * https://github.com/osptek
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "aster_idf_runtime.h"
#include "aster_idf_wifi.h"
#include "aster_idf_apps.h"
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"


#include "esp_lcd_st7102.h"
#include "esp_lcd_touch_st7123.h"
#include "bootloader_random.h"



/* LCD size */
#define EXAMPLE_LCD_H_RES   (480)
#define EXAMPLE_LCD_V_RES   (480)

#define MIPI_DPI_PX_FORMAT LCD_COLOR_FMT_RGB565
#define BSP_LCD_COLOR_DEPTH 16

// “VDD_MIPI_DPHY”应供电 2.5V，可从内部 LDO 稳压器或外部 LDO 芯片获取电源
#define EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN 3 // LDO_VO3 连接至 VDD_MIPI_DPHY
#define EXAMPLE_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL 1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL

#define EXAMPLE_PIN_NUM_BK_LIGHT CONFIG_ASTER_P4_BACKLIGHT_GPIO
#define EXAMPLE_PIN_NUM_LCD_RST  CONFIG_ASTER_P4_LCD_RESET_GPIO

/* Touch settings */
#define EXAMPLE_TOUCH_I2C_NUM       (1)
#define EXAMPLE_TOUCH_I2C_CLK_HZ    (100000)

/* LCD touch pins */
#define EXAMPLE_TOUCH_I2C_SCL       (CONFIG_ASTER_P4_TOUCH_SCL_GPIO)
#define EXAMPLE_TOUCH_I2C_SDA       (CONFIG_ASTER_P4_TOUCH_SDA_GPIO)
#define EXAMPLE_TOUCH_RST           (CONFIG_ASTER_P4_TOUCH_RESET_GPIO)
#define EXAMPLE_TOUCH_INT           (CONFIG_ASTER_P4_TOUCH_INT_GPIO)

static const char *TAG = "EXAMPLE";

/* LCD IO and panel */
static esp_lcd_panel_handle_t lcd_panel = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;
static const uint8_t *scanout;
static const uint8_t *capture_scanout(void) {return scanout;}

static void example_bsp_enable_dsi_phy_power(void)
{
    // 打开 MIPI DSI PHY 的电源，使其从“无电”状态进入“关机”状态
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
#ifdef EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = EXAMPLE_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
#endif
}

static void example_bsp_init_lcd_backlight(void)
{
#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_BK_LIGHT};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
#endif
}

static void example_bsp_set_lcd_backlight(uint32_t level)
{
#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, level);
#endif
}

static const st7102_lcd_init_cmd_t lcd_init_cmds[] = {
//  {cmd, { data }, data_size, delay_ms}
    {0x99, (uint8_t []){0x71,0x02,0xa2}, 3, 0},
    {0x99, (uint8_t []){0x71,0x02,0xa3}, 3, 0},
    {0x99, (uint8_t []){0x71,0x02,0xa4}, 3, 0},
    {0xB0, (uint8_t []){0x22,0x61,0x1E,0x61,0x2F,0x39,0x39}, 7, 0},
    {0xB7, (uint8_t []){0x46,0x46}, 2, 0},
    {0xBF, (uint8_t []){0x50,0x50}, 2, 0},
    {0xD7, (uint8_t []){0x00,0x0C,0x80,0x08,0xF0,0xF0}, 6, 0},
    {0xA3, (uint8_t []){0x40,0x03,0x80,0x00,0x44,0x00,0x00,0x00,0x00,0x04,
                        0x6F,0x6F,0x00,0x1A,0x00,0x45,0x05,0x00,0x00,0x00,
                        0x00,0x46,0x00,0x00,0x12,0x20,0x52,0x00,0x05,0x00,
                        0x00,0xFF}, 32, 0},
    {0xA6, (uint8_t []){0x08,0x00,0x24,0x55,0x35,0x00,0x76,0x40,0x58,0x58,
                        0x00,0x24,0x55,0x00,0x00,0x40,0x40,0x58,0x58,0x02,
                        0xAC,0x51,0x00,0xCC,0x40,0x40,0x58,0x58,0x00,0xAC,
                        0x11,0x00,0x00,0x40,0x40,0x58,0x58,0x00,0x00,0x06,
                        0x00,0x00,0x00,0x00}, 44, 0},
    {0xA7, (uint8_t []){0x19,0x19,0x00,0x64,0x40,0x07,0x16,0x40,0x00,0x44,
                        0x43,0x58,0x58,0x00,0x64,0x40,0x25,0x34,0x00,0x00,
                        0x42,0x41,0x58,0x58,0x00,0x64,0x40,0x4B,0x5A,0x00,
                        0x00,0x42,0x41,0x58,0x58,0x00,0x24,0x40,0x69,0x78,
                        0x00,0x00,0x40,0x40,0x58,0x58,0x00,0x44}, 48, 0},
    {0xAC, (uint8_t []){0x00,0x1C,0x04,0x1A,0x19,0x1B,0x1B,0x18,0x06,0x13,
                        0x19,0x11,0x1B,0x08,0x18,0x0A,0x01,0x1C,0x04,0x1A,
                        0x19,0x01,0x1B,0x18,0x06,0x12,0x19,0x10,0x1B,0x09,
                        0x18,0x0B,0xBF,0xAA,0xBF,0xAA,0x00}, 37, 0},
    {0xAD, (uint8_t []){0xCC,0x40,0x46,0x11,0x04,0x6F,0x6F}, 7, 0},
    {0xE8, (uint8_t []){0x30,0x07,0x05,0x74,0x74,0x9C,0x00,0xE2,0x04,0x00,
                        0x00,0x00,0x00,0xEF}, 14, 0},
    {0x75, (uint8_t []){0x03,0x04}, 2, 0},
    {0xE7, (uint8_t []){0x8B,0x3C,0x00,0x0C,0xF0,0x5D,0x00,0x5D,0x00,0x5D,
                        0x00,0x5D,0x00,0xFF,0x00,0x08,0x7B,0x00,0x00,0xC8,
                        0x6A,0x5A,0x08,0x1A,0x3C,0x00,0xA1,0x01,0x8C,0x01,
                        0x7F,0xF0,0x22}, 33, 0},
    {0xE9, (uint8_t []){0x3C,0x7F,0x08,0x07,0x1A,0x7A,0x22,0x1A,0x33}, 9, 0},
    {0xC8, (uint8_t []){0x00,0x00,0x15,0x26,0x44,0x00,0x78,0x03,0xBE,0x06,
                        0x11,0x1C,0x09,0x8A,0x03,0x21,0xD4,0x01,0x11,0x0F,
                        0x22,0x4A,0x0F,0x8F,0x0A,0x32,0xF0,0x0A,0x41,0x0D,
                        0xF3,0x80,0x0D,0xAE,0xC5,0x03,0xC4}, 37, 0},
    {0xC9, (uint8_t []){0x00,0x00,0x15,0x26,0x44,0x00,0x78,0x03,0xBE,0x06,
                        0x11,0x1C,0x09,0x8A,0x03,0x21,0xD4,0x01,0x11,0x0F,
                        0x22,0x4A,0x0F,0x8F,0x0A,0x32,0xF0,0x0A,0x41,0x0D,
                        0xF3,0x80,0x0D,0xAE,0xC5,0x03,0xC4}, 37, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 20},
};

static esp_err_t app_lcd_init(void)
{
    esp_err_t ret = ESP_OK;

    example_bsp_enable_dsi_phy_power();
    example_bsp_init_lcd_backlight();
    example_bsp_set_lcd_backlight(EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL);

    // 首先创建 MIPI DSI 总线，它还将初始化 DSI PHY
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = {                    \
        .bus_id = 0,                                           \
        .num_data_lanes = 2,                                   \
        /* IDF selects XTAL for rev3 and the legacy source for pre3. */
        .phy_clk_src = 0,                                     \
        .lane_bit_rate_mbps = 620,                             \
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus), err, TAG, "LCD init failed");

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    // 我们使用DBI接口发送LCD命令和参数
    esp_lcd_dbi_io_config_t dbi_config = ST7102_MIPI_PANEL_IO_DBI_CONFIG();

    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io_handle), err, TAG, "LCD init failed");

    /* ST7102 会在 new_panel 时立刻 DBI 读 ID；须先释放/复位 RST，否则读 FIFO 会空转卡死 */
#if EXAMPLE_PIN_NUM_LCD_RST >= 0
    gpio_config_t lcd_rst_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_LCD_RST,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&lcd_rst_gpio_config), err, TAG, "LCD RST GPIO config failed");
    gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
#endif
#if EXAMPLE_TOUCH_RST >= 0
    /* ST7123 为 TDDI，CTP RST 需在 LCD 初始化前保持释放 */
    gpio_config_t tp_rst_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_TOUCH_RST,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&tp_rst_gpio_config), err, TAG, "Touch RST GPIO config failed");
    gpio_set_level(EXAMPLE_TOUCH_RST, 1);
#endif

    // 创建ST7102控制面板
    esp_lcd_dpi_panel_config_t dpi_config = {                 \
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,          \
        .dpi_clock_freq_mhz = 30,                             \
        .virtual_channel = 0,                                 \
        .in_color_format = MIPI_DPI_PX_FORMAT,
        .out_color_format = MIPI_DPI_PX_FORMAT,                   \
        .num_fbs = 1,                                         \
        .video_timing = {                                     \
            .h_size = 480,                                    \
            .v_size = 480,                                    \
            .hsync_back_porch = 140,                          \
            .hsync_pulse_width = 2,                           \
            .hsync_front_porch = 120,                         \
            .vsync_back_porch = 8,                            \
            .vsync_pulse_width = 4,                           \
            .vsync_front_porch = 200,                         \
        },                                                    \
                                      \
    };

    st7102_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,      // Uncomment these line if use custom initialization commands
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(st7102_lcd_init_cmd_t),
        .flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_COLOR_DEPTH,
        .vendor_config = &vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7102(io_handle, &panel_config, &lcd_panel), err, TAG, "LCD init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(lcd_panel), err, TAG, "LCD init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(lcd_panel), err, TAG, "LCD init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(lcd_panel, true), err, TAG, "LCD init failed");

    // 打开背光
    example_bsp_set_lcd_backlight(EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);

    return ret;

err:
    if (lcd_panel) {
        esp_lcd_panel_del(lcd_panel);
    }
    return ret;
}

static esp_err_t app_touch_init(void)
{
    /* TDDI：LCD 起来后先给触摸足够复位/就绪时间，避免驱动内仅等 10ms 就读版本失败 */
#if EXAMPLE_TOUCH_RST >= 0
    gpio_config_t tp_rst_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_TOUCH_RST,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&tp_rst_gpio_config), TAG, "Touch RST GPIO config failed");
    gpio_set_level(EXAMPLE_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EXAMPLE_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
#endif

    const i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = EXAMPLE_TOUCH_I2C_NUM,
        .scl_io_num = EXAMPLE_TOUCH_I2C_SCL,
        .sda_io_num = EXAMPLE_TOUCH_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_config, &i2c_bus), TAG, "I2C bus init failed");

    esp_err_t probe = i2c_master_probe(i2c_bus, ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS, 100);
    ESP_LOGI(TAG, "ST7123 I2C probe 0x%02X: %s", ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS, esp_err_to_name(probe));

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
    tp_io_config.scl_speed_hz = EXAMPLE_TOUCH_I2C_CLK_HZ;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io_handle), TAG, "Touch panel IO failed");

    /* RST 已在上方手动处理并加长等待，这里传 NC 避免驱动再次短复位 */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = EXAMPLE_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    return esp_lcd_touch_new_i2c_st7123(tp_io_handle, &tp_cfg, &touch_handle);
}


/* IDF 6 DPI panel's CPU-copy draw path is synchronous (no DMA copy hook).
 * No WASM pointer is retained after this callback returns. */
static int present_pixels(const uint8_t *pixels, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, uint32_t pitch)
{
    if (pitch == w)
        return esp_lcd_panel_draw_bitmap(lcd_panel, x, y, x+w, y+h, pixels) == ESP_OK ? 0 : -1;
    for (uint32_t row=0; row<h; ++row)
        if (esp_lcd_panel_draw_bitmap(lcd_panel, x, y+row, x+w, y+row+1,
                                     pixels+(size_t)row*pitch*2) != ESP_OK) return -1;
    return 0;
}

static int read_touch(uint32_t *x, uint32_t *y)
{
    if (esp_lcd_touch_read_data(touch_handle) != ESP_OK) return -1;
    uint16_t tx, ty, strength;
    uint8_t count=0;
    if (!esp_lcd_touch_get_coordinates(touch_handle, &tx, &ty, &strength, &count, 1)) return 0;
    if (!count) return 0;
    *x=tx; *y=ty;
    return 1;
}

/* Experimental board bring-up plus the core Aster lifecycle subset. */
void osptek_storage_start(void);
void app_main(void)
{
    /* P4 has no local RF entropy source: C5 Wi-Fi is a different chip.
     * This board profile reserves P4's SAR ADC for hardware RNG entropy;
     * microphone capture uses the external ES8311 over I2S. */
    bootloader_random_enable();
    ESP_LOGW(TAG, "OSPTEK P4 bring-up; baseboard V1.5 schematic pinout, not hardware-tested");
    ESP_ERROR_CHECK(app_lcd_init());
    uint16_t *pixels = heap_caps_malloc(480 * 480 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        ESP_LOGE(TAG, "Cannot allocate RGB565 test image");
        return;
    }
    const uint16_t bars[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0x0000, 0xffe0};
    for (int y = 0; y < 480; ++y)
        for (int x = 0; x < 480; ++x)
            pixels[y * 480 + x] = bars[x / 80];
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, 480, 480, pixels));
    /* Retain the buffer until shutdown; the panel transfer may be asynchronous. */
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t ret = app_touch_init();
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "Touch unavailable: %s; display remains available", esp_err_to_name(ret));
    aster_idf_io_t io = {
        .width=480, .height=480, .present=present_pixels,
        .touch=ret == ESP_OK ? read_touch : NULL,
    };
    /* The runtime owns touch reads; do not poll the controller from app_main. */
    aster_idf_set_io(&io);
    void *frame=NULL;
    if(esp_lcd_dpi_panel_get_frame_buffer(lcd_panel,1,&frame)==ESP_OK) {
        scanout=frame;aster_idf_set_capture(capture_scanout);
    }
    ESP_LOGI(TAG, "Aster RGB565 display ready; touch=%s", io.touch ? "ready" : "unavailable");
    osptek_storage_start();
    extern int aster_idf_audio_start(void);
    int audio=aster_idf_audio_start();
    if(audio)ESP_LOGW(TAG,"Audio unavailable: %d",audio);
    int apps=aster_idf_apps_start();
    if(apps)ESP_LOGE(TAG,"Application worker unavailable: %d",apps);
    aster_idf_set_app_command(aster_idf_apps_command);
    aster_idf_network_start();
}
