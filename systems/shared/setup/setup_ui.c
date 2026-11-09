/* SPDX-License-Identifier: MIT */
#include "setup_ui.h"
#include "aster_setup.h"
#include "aster_platform.h"
#include "aster_mem.h"
#include "ui_lvgl.h"
#include "ui_splash.h"
#include "system_font.h"
#include "font_sd.h"
#include "qrcodegen.h"
#include "launcher.h"
#include <aster/event.h>
#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>
static lv_obj_t *root, *heading, *message, *bar, *numbers, *detail;
static int page = -1;
static uint32_t last_draw;
static uint8_t *qr_pixels;
static lv_image_dsc_t qr_image;
static aster_setup_snapshot status;
static lv_obj_t *label(lv_obj_t *parent, const char *value, int x, int y, int w, unsigned px,
                       uint32_t color) {
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_font(o, aster_system_font(px), 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
    lv_label_set_text(o, value);
    return o;
}
static void click(lv_event_t *event) {
    intptr_t command = (intptr_t)lv_event_get_user_data(event);
    if (command == 1)
        aster_setup_begin();
    else
        aster_setup_retry();
}
static void button(const char *text, int command, int w, int h) {
    lv_obj_t *o = lv_button_create(root);
    lv_obj_set_size(o, w - 40, 48);
    lv_obj_set_pos(o, 20, h - 68);
    lv_obj_set_style_bg_color(o, lv_color_hex(0xd8ff8c), 0);
    lv_obj_set_style_radius(o, 16, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_add_event_cb(o, click, LV_EVENT_CLICKED, (void *)(intptr_t)command);
    lv_obj_t *t = label(o, text, 0, 0, w - 60, 20, 0x211d28);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(t);
}
static void qr(int w, int y) {
    char value[160];
    snprintf(value, sizeof(value), "WIFI:T:WPA;S:%s;P:%s;;", status.ssid, status.password);
    uint8_t temporary[qrcodegen_BUFFER_LEN_FOR_VERSION(6)],
        code[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    if (!qrcodegen_encodeText(value, temporary, code, qrcodegen_Ecc_MEDIUM, 1, 6,
                              qrcodegen_Mask_AUTO, true))
        return;
    unsigned modules = qrcodegen_getSize(code) + 8;
    unsigned physical_w = aster_ui_lvgl_width(), physical_h = aster_ui_lvgl_height();
    unsigned physical = physical_w < 360 ? 112 : 152;
    unsigned qw = physical * w / physical_w,
             qh = physical * aster_ui_lvgl_layout_height() / physical_h;
    qr_pixels = aster_psram_alloc((size_t)qw * qh * 2);
    if (!qr_pixels)
        return;
    for (unsigned row = 0; row < qh; row++)
        for (unsigned col = 0; col < qw; col++) {
            int mx = col * modules / qw - 4, my = row * modules / qh - 4;
            uint16_t p = qrcodegen_getModule(code, mx, my) ? 0 : 0xffff;
            memcpy(qr_pixels + 2 * ((size_t)row * qw + col), &p, 2);
        }
    qr_image = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC,
                                           .cf = LV_COLOR_FORMAT_RGB565,
                                           .w = qw,
                                           .h = qh,
                                           .stride = qw * 2},
                                .data_size = qw * qh * 2,
                                .data = qr_pixels};
    lv_obj_t *image = lv_image_create(root);
    lv_image_set_src(image, &qr_image);
    lv_obj_set_pos(image, (w - qw) / 2, y);
}
static void rebuild(int next) {
    if (root)
        lv_obj_delete(root);
    if (qr_pixels) {
        aster_psram_free(qr_pixels);
        qr_pixels = NULL;
    }
    aster_splash_off();
    aster_ui_lvgl_resync_window();
    int w = aster_ui_lvgl_layout_width(), h = aster_ui_lvgl_layout_height();
    root = lv_obj_create(aster_ui_lvgl_screen());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, w, h);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x131317), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(root, 0, 0);
    label(root, "aster", 24, 18, w - 48, 24, 0xd8ff8c);
    label(root,
          next == ASTER_SETUP_WELCOME     ? "01 / 03"
          : next == ASTER_SETUP_PROVISION ? "02 / 03"
                                          : "03 / 03",
          w - 105, 24, 85, 16, 0xa0ae92);
    heading =
        label(root, aster_setup_phase_title(status.state.phase), 24, 64, w - 48, 24, 0xf3f3e8);
    message = label(root, "", 24, 104, w - 48, 16, 0xa0ae92);
    bar = numbers = detail = NULL;
    page = next;
    if (next == ASTER_SETUP_WELCOME) {
        lv_label_set_text(message,h<400?
            "首次使用只需几步\n\n连接网络后，自动下载\n系统字体和商城。\n请勿移除存储卡。":
            "首次使用只需几步\n\n连接网络，下载字体和商城。\n请勿移除存储卡。");
        if(h>=400)label(root, "Wi-Fi  /  字体  /  商城", 24, h / 2 + 24, w - 48, 20, 0xd8ff8c);
        button("开始设置", 1, w, h);
    } else if (next == ASTER_SETUP_STORAGE) {
        lv_label_set_text(message, "请插入存储卡\n\n系统需要一张可读写的存储卡。");
        button("重新检查", 2, w, h);
    } else if (next == ASTER_SETUP_PROVISION) {
        lv_label_set_text(message, h<400?"扫码连接热点":"连接下方热点后打开配网页面");
        int qr_y=h<400?128:139;
        qr(w, qr_y);
        int qh = (aster_ui_lvgl_width() < 360 ? 112 : 152) * h / aster_ui_lvgl_height();
        char text[192];
        snprintf(text, sizeof(text), "热点  %s\n密码  %s\n地址  http://192.168.4.1", status.ssid,
                 status.password);
        label(root, text, 24, qr_y+12+qh, w - 48, 16, 0xf3f3e8);
    } else if (next == ASTER_SETUP_ERROR) {
        char text[256];
        const char *reason = status.state.error == -28                          ? "存储空间不足"
                             : status.state.failed_phase == ASTER_SETUP_CONNECT ? "网络连接失败"
                             : status.state.failed_phase == ASTER_SETUP_VERIFY
                                 ? "资源校验失败"
                                 : "请检查网络、服务器和存储卡";
        snprintf(text, sizeof(text), "%s\n\n错误 %d\n已下载文件会保留。", reason,
                 status.state.error);
        lv_label_set_text(message, text);
        button("重新连接", 2, w, h);
    } else {
        lv_label_set_text(message,h<400?"请保持供电，勿移除卡。\n可离开手机页面。":
            "请保持供电，请勿移除存储卡。\n此过程可离开手机页面。");
        bar = lv_bar_create(root);
        lv_obj_set_size(bar, w - 48, 12);
        lv_obj_set_pos(bar, 24, h / 2);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x33392d), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xd8ff8c), LV_PART_INDICATOR);
        numbers = label(root, "", 24, h / 2 + 30, w - 48, 20, 0xf3f3e8);
        detail = label(root, "", 24, h / 2 + 90, w - 48, 16, 0xa0ae92);
    }
    aster_ui_lvgl_pointer(0, 0, 0);
    lv_indev_reset(aster_ui_lvgl_indev(), NULL);
}
bool aster_setup_ui_tick(void) {
    if(aster_setup_format_pending()) {
        aster_launcher_hide();aster_font_sd_cache_drop();aster_setup_format_ui_ready();
    }
    if (!aster_setup_poll(&status)) {
        if (root) {
            lv_obj_delete(root);
            root = NULL;
            heading = message = bar = numbers = detail = NULL;
            page = -1;
            if(status.state.files)aster_font_sd_reload();
        }
        if (qr_pixels) {
            aster_psram_free(qr_pixels);
            qr_pixels = NULL;
        }
        return false;
    }
    if (!aster_ui_lvgl_screen())
        return true;
    int next = status.state.phase == ASTER_SETUP_VERIFY ? ASTER_SETUP_DOWNLOAD : status.state.phase;
    if (next != page)
        rebuild(next);
    uint32_t now = k_uptime_get_32();
    if (now - last_draw >= 150) {
        last_draw = now;
        lv_label_set_text(heading, aster_setup_phase_title(status.state.phase));
        if (bar)
            lv_bar_set_value(bar, aster_setup_core_percent(&status.state), LV_ANIM_OFF);
        if (numbers) {
            char text[160];
            snprintf(text, sizeof(text), "%u%%    %llu / %llu MiB",
                     aster_setup_core_percent(&status.state),
                     (unsigned long long)(status.state.bytes / 1048576),
                     (unsigned long long)(status.state.total / 1048576));
            lv_label_set_text(numbers, text);
        }
        if (detail) {
            char text[192];
            snprintf(text, sizeof(text), "文件 %u / %u  ·  用时 %u s\n%s",
                     (unsigned)status.state.file, (unsigned)status.state.files,
                     (unsigned)status.seconds, status.file);
            lv_label_set_text(detail, text);
        }
    }
    aster_touch_evt_t events[ASTER_PLAT_TOUCH_EVENT_QUEUE];
    int n = aster_plat_input_poll(events, ASTER_PLAT_TOUCH_EVENT_QUEUE);
    for (int i = 0; i < n; i++)
        if (events[i].slot == 0) {
            aster_ui_lvgl_pointer(events[i].x, events[i].y, events[i].action != ASTER_TOUCH_UP);
            lv_indev_read(aster_ui_lvgl_indev());
        }
    aster_ui_lvgl_pump();
    return true;
}
