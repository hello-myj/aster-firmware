/* SPDX-License-Identifier: MIT */
#include "aster_power_ui.h"
#include "aster_device.h"
#include "aster_os.h"
#include "launcher.h"
#include "qpanel.h"
#include "font_sd.h"
#include "system_font.h"
#include "ui_lvgl.h"
#include "aster_platform.h"
#include <lvgl.h>
#include <stdio.h>
static lv_obj_t *root, *detail;
static lv_obj_t *usb_cancel_button;
static bool usb_started, usb_canceled, usb_connected;

static void close_status(lv_event_t *event)
{
    (void)event;
    if (root)
        lv_obj_delete(root);
    root = detail = NULL;
    aster_launcher_request_show();
}

static lv_obj_t *label(const char *text, int y, unsigned size)
{
    lv_obj_t *o = lv_label_create(root);
    lv_obj_set_pos(o, 24, y);
    lv_obj_set_width(o, aster_ui_lvgl_width() - 48);
    lv_obj_set_style_text_font(o, aster_system_font(size), 0);
    lv_obj_set_style_text_color(o, lv_color_hex(0xf3f3e8), 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
    lv_label_set_text(o, text);
    return o;
}

static void cancel_usb(lv_event_t *event)
{
    (void)event;
    if (!usb_connected)
        usb_canceled = true;
}

bool aster_power_ui_usb_tick(bool connected, const char *connection_hint)
{
    if (!root || !detail)
        return false;
    if (!usb_started) {
        usb_started = true;
        usb_cancel_button = lv_button_create(root);
        lv_obj_set_pos(usb_cancel_button, 24, aster_ui_lvgl_height() - 88);
        lv_obj_set_size(usb_cancel_button, aster_ui_lvgl_width() - 48, 56);
        lv_obj_t *text = lv_label_create(usb_cancel_button);
        lv_obj_set_style_text_font(text, aster_system_font(20), 0);
        lv_label_set_text(text, "取消");
        lv_obj_center(text);
        lv_obj_add_event_cb(usb_cancel_button, cancel_usb, LV_EVENT_CLICKED, NULL);
        aster_ui_lvgl_pointer(0, 0, 0);
        lv_indev_reset(aster_ui_lvgl_indev(), NULL);
        lv_label_set_text(detail, connection_hint);
    }
    if (connected && !usb_connected) {
        usb_connected = true;
        lv_obj_add_flag(usb_cancel_button, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(detail, "电脑正在使用存储卡。完成后，请在电脑安全弹出，再拔线。");
    }
    aster_touch_evt_t events[ASTER_PLAT_TOUCH_EVENT_QUEUE];
    int n = aster_device_input_poll(events, ASTER_PLAT_TOUCH_EVENT_QUEUE);
    for (int i = 0; i < n; i++)
        if (events[i].slot == 0) {
            aster_ui_lvgl_pointer(events[i].x, events[i].y, events[i].action != ASTER_TOUCH_UP);
            lv_indev_read(aster_ui_lvgl_indev());
        }
    aster_ui_lvgl_pump();
    return usb_canceled;
}

void aster_power_ui_update(const aster_power_request_t *request, unsigned stage, int result)
{
    if (!aster_ui_lvgl_screen())
        return;
    if (stage == 0) {
        if (root) {
            lv_obj_delete(root);
            root = detail = NULL;
        }
        usb_started = usb_canceled = usb_connected = false;
        aster_launcher_power_resume();
        aster_launcher_hide();
        aster_qpanel_force_close();
        root = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(root);
        lv_obj_set_pos(root, 0, 0);
        lv_obj_set_size(root, aster_ui_lvgl_width(), aster_ui_lvgl_height());
        lv_obj_set_style_bg_color(root, lv_color_hex(0x181b18), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        label(request->mode == ASTER_POWER_RESTART       ? "正在重启"
              : request->mode == ASTER_POWER_SHUTDOWN    ? "正在关机"
              : request->mode == ASTER_POWER_USB_STORAGE ? "USB 读卡器"
                                                         : "准备休眠",
              60, 24);
        detail = label("正在停止应用和音频", 130, 20);
    }
    if (stage == 1 && detail)
        lv_label_set_text(detail, "正在等待传输与安装结束");
    if (stage == 2 && detail)
        lv_label_set_text(detail, "正在同步存储，请勿断电");
    if (stage == 4) {
        aster_launcher_power_resume();
        if (result) {
            if (detail) {
                char text[120];
                snprintf(text, sizeof(text), "操作未完成 (%d)\n请稍后再试", result);
                lv_label_set_text(detail, text);
            }
            if (root) {
                lv_obj_t *b = lv_button_create(root);
                lv_obj_set_pos(b, 24, aster_ui_lvgl_height() - 88);
                lv_obj_set_size(b, aster_ui_lvgl_width() - 48, 56);
                lv_obj_t *t = lv_label_create(b);
                lv_obj_set_style_text_font(t, aster_system_font(20), 0);
                lv_label_set_text(t, "返回桌面");
                lv_obj_center(t);
                lv_obj_add_event_cb(b, close_status, LV_EVENT_CLICKED, NULL);
            }
        } else
            close_status(NULL);
    }
    aster_ui_lvgl_request_frame();
    aster_ui_lvgl_pump();
    if (stage == 2) {
        aster_device_display_flush_drain();
        aster_font_sd_cache_drop();
        /* Give the user time to read the final on-screen status before power cut. */
        if (request->mode == ASTER_POWER_SHUTDOWN)
            aster_os_sleep_ms(1800);
        if (request->mode & (ASTER_POWER_LIGHT_SLEEP | ASTER_POWER_DEEP_SLEEP))
            aster_device_backlight_set(false, 0);
    }
}
