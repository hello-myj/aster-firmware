/* SPDX-License-Identifier: MIT */
#include "aster_idf_ui.h"
#include "aster_idf_runtime.h"
#include "aster_platform.h"
#include "ui_lvgl.h"
#include "ui_font.h"
#include "font_sd.h"
#include "launcher.h"
#include "qpanel.h"
#include "wifi_settings.h"
#include "gesture.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
static lv_display_t *display;
static lv_indev_t *pointer;
static uint32_t pointer_x,pointer_y;
static bool pressed,app_active;
static int (*launch_callback)(const char *);
static uint32_t tick(void) {return esp_timer_get_time()/1000;}
static void flush(lv_display_t *d,const lv_area_t *area,uint8_t *pixels)
{
    if(!app_active || aster_qpanel_app_holds_glass()) {
        uint32_t w=area->x2-area->x1+1,h=area->y2-area->y1+1;
        aster_plat_display_write_at(pixels,area->x1,area->y1,w,h,w);
    }
    lv_display_flush_ready(d);
}
static void read_pointer(lv_indev_t *indev,lv_indev_data_t *out)
{(void)indev;out->point.x=pointer_x;out->point.y=pointer_y;out->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
int aster_ui_lvgl_init(void)
{
    if(display)return 0;
    extern void aster_idf_render_init(void);aster_idf_render_init();
    aster_display_info_t info;if(aster_plat_display_get_info(0,&info))return ASTER_ENODEV;
    size_t bytes=info.width*40*2;
    void *a=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    void *b=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!a || !b){free(a);free(b);return ASTER_ENOMEM;}
    display=lv_display_create(info.width,info.height);
    if(!display){free(a);free(b);return ASTER_ENOMEM;}
    lv_tick_set_cb(tick);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,a,b,bytes,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    pointer=lv_indev_create();
    if(!pointer){lv_display_delete(display);display=NULL;free(a);free(b);return ASTER_ENOMEM;}
    lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(pointer,read_pointer);
    aster_font_sd_init();return 0;
}
void aster_ui_lvgl_pointer(uint32_t x,uint32_t y,int down) {pointer_x=x;pointer_y=y;pressed=down!=0;}
void aster_ui_lvgl_pump(void) {if(display)lv_timer_handler();}
void aster_ui_lvgl_request_frame(void) {if(display)lv_obj_invalidate(lv_screen_active());}
lv_obj_t *aster_ui_lvgl_screen(void) {return display?lv_screen_active():NULL;}
uint32_t aster_ui_lvgl_width(void) {return display?lv_display_get_horizontal_resolution(display):0;}
uint32_t aster_ui_lvgl_height(void) {return display?lv_display_get_vertical_resolution(display):0;}
uint32_t aster_ui_lvgl_layout_width(void) {return aster_ui_lvgl_width();}
uint32_t aster_ui_lvgl_layout_height(void) {return aster_ui_lvgl_height();}
void aster_ui_lvgl_resync_window(void) {}
lv_indev_t *aster_ui_lvgl_indev(void) {return pointer;}
int aster_runtime_launch_async(const char *id) {return launch_callback?launch_callback(id):ASTER_ESTATE;}
static bool touch(void *value)
{
    aster_event_t *event=value;bool down=event->u.touch.action!=ASTER_TOUCH_UP;
    bool was_open=aster_qpanel_is_open();
    aster_gest_t gesture=aster_gesture_feed(event->u.touch.x,event->u.touch.y,down,
        aster_qpanel_allow(false),aster_ui_lvgl_height());
    bool consumed=aster_qpanel_on_gesture(gesture,ASTER_QPANEL_OVER_APP);
    if(!consumed && !aster_qpanel_is_open() &&
       aster_launcher_app_touch(event->u.touch.x,event->u.touch.y,down)) {
        aster_qpanel_request_exit("应用");consumed=true;
    }
    if(gesture==ASTER_GEST_PANEL_UP){aster_qpanel_request_exit("应用");consumed=true;}
    if(was_open || aster_qpanel_is_open() || consumed) {
        aster_ui_lvgl_pointer(event->u.touch.x,event->u.touch.y,down);
        return true;
    }
    return false;
}
static void app_tick(void)
{
    aster_ui_fonts_advance();aster_qpanel_tick();
    if(aster_qpanel_is_open())aster_ui_lvgl_pump();
    if(aster_qpanel_take_exit_confirmed())aster_idf_exit_current(0);
}
int aster_idf_ui_init(int (*launch)(const char *))
{
    launch_callback=launch;int r=aster_ui_lvgl_init();if(r)return r;
    aster_launcher_init();aster_launcher_request_show();
    const aster_idf_ui_hooks_t hooks={touch,app_tick,aster_qpanel_app_holds_glass};
    aster_idf_set_ui_hooks(&hooks);return 0;
}
void aster_idf_ui_idle(void) {if(display)aster_launcher_pump_idle();}
void aster_idf_ui_before_app(void)
{app_active=true;aster_launcher_hide();aster_qpanel_force_close();aster_gesture_reset();aster_launcher_app_touch_cancel();}
void aster_idf_ui_after_app(void)
{app_active=false;aster_qpanel_on_app_stop();aster_launcher_request_show();}
