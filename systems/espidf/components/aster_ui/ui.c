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
#include "flux_draw.h"
#ifndef ASTER_IDF_HOST_TEST
#include "ppa_fill.h"
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static lv_display_t *display;
static lv_indev_t *pointer;
static uint32_t pointer_x,pointer_y;
static bool pressed,app_active;
static int (*launch_callback)(const char *);
static atomic_int perf_request;
static struct {
    uint64_t since,render_start,last_frame,render_us,flush_us,bytes,pump_us;
    uint32_t frames,flushes,max_render_us,max_pump_us,interval[256];
} perf;
static uint64_t bench_warm,bench_end,bench_step;
static void perf_reset(void)
{
    memset(&perf,0,sizeof(perf));perf.since=esp_timer_get_time();aster_flux_draw_time_reset();
#ifndef ASTER_IDF_HOST_TEST
    aster_idf_ppa_fill_reset();
#endif
}
static void perf_print(void)
{
    uint32_t bakes,hits,cached;aster_launcher_art_stats(&bakes,&hits,&cached);
    uint64_t elapsed=esp_timer_get_time()-perf.since;
    unsigned p95=0,total=0,goal=perf.frames>1?(perf.frames-1)*95/100:0;
    for(unsigned i=0;i<256;i++){total+=perf.interval[i];if(total>=goal){p95=i;break;}}
    printf("UI PERF frames=%lu elapsed_ms=%llu fps_x10=%llu render_avg_us=%llu render_max_us=%lu "
           "interval_p95_ms=%u flush_us=%llu flushes=%lu bytes=%llu pump_us=%llu pump_max_us=%lu ribbon_us=%lu\n",
           (unsigned long)perf.frames,(unsigned long long)(elapsed/1000),
           (unsigned long long)(elapsed?(uint64_t)perf.frames*10000000/elapsed:0),
           (unsigned long long)(perf.frames?perf.render_us/perf.frames:0),(unsigned long)perf.max_render_us,p95,
           (unsigned long long)perf.flush_us,(unsigned long)perf.flushes,(unsigned long long)perf.bytes,
           (unsigned long long)perf.pump_us,(unsigned long)perf.max_pump_us,(unsigned long)aster_flux_draw_time_reset());
    printf("UI ART bakes=%lu hits=%lu inactive_bytes=%lu\n",(unsigned long)bakes,(unsigned long)hits,(unsigned long)cached);
#ifndef ASTER_IDF_HOST_TEST
    aster_idf_ppa_fill_report();
#endif
}
static void perf_event(lv_event_t *event)
{
    uint64_t now=esp_timer_get_time();
    if(lv_event_get_code(event)==LV_EVENT_RENDER_START) {
        perf.render_start=now;
        if(perf.last_frame){unsigned ms=(now-perf.last_frame)/1000;perf.interval[ms<255?ms:255]++;}
        perf.last_frame=now;
    } else {
        uint32_t us=(uint32_t)(now-perf.render_start);perf.frames++;perf.render_us+=us;
        if(us>perf.max_render_us)perf.max_render_us=us;
    }
}
int aster_idf_ui_command(int argc,char **argv)
{
    int request;
    if(argc==3 && !strcmp(argv[2],"perf"))request=1;
    else if((argc==3 || argc==4) && !strcmp(argv[2],"bench")) {
        char *end=NULL;long seconds=argc==4?strtol(argv[3],&end,10):8;
        if(seconds<2 || seconds>20 || (end && *end))return ASTER_EINVAL;
        request=100+(int)seconds;
    } else return ASTER_EINVAL;
    int empty=0;return atomic_compare_exchange_strong(&perf_request,&empty,request)?0:ASTER_EBUSY;
}
static void perf_tick(void)
{
    uint64_t now=esp_timer_get_time();int request=atomic_exchange(&perf_request,0);
    if(request==1)perf_print();
    else if(request>100) {
        if(app_active){puts("UI BENCH busy: stop the application first");return;}
        aster_launcher_show_page(0);bench_step=now;bench_warm=now+2000000;
        bench_end=bench_warm+(request-100)*1000000ULL;
        printf("UI BENCH warmup=2s measurement=%ds\n",request-100);
    }
    if(!bench_end)return;
    if(now>=bench_end){bench_end=0;perf_print();puts("UI BENCH done");return;}
    if(bench_warm && now>=bench_warm){bench_warm=0;perf_reset();}
    if(now>=bench_step){aster_launcher_step_page(1);bench_step=now+180000;}
}
static uint32_t tick(void) {return esp_timer_get_time()/1000;}
static void flush(lv_display_t *d,const lv_area_t *area,uint8_t *pixels)
{
    if(!app_active || aster_qpanel_app_holds_glass()) {
        uint32_t w=area->x2-area->x1+1,h=area->y2-area->y1+1;
        uint64_t start=esp_timer_get_time();
        aster_plat_display_write_at(pixels,area->x1,area->y1,w,h,w);
        perf.flush_us+=esp_timer_get_time()-start;perf.flushes++;perf.bytes+=(uint64_t)w*h*2;
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
    /* One screen-sized partial buffer avoids replaying the same transformed
     * cards for twelve short bands. Flush remains synchronous, so a second
     * draw buffer cannot overlap work and only reserves extra PSRAM. */
    size_t bytes=((size_t)info.width*info.height*2+63)&~(size_t)63;
    void *a=heap_caps_aligned_alloc(64,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!a)return ASTER_ENOMEM;
    display=lv_display_create(info.width,info.height);
    if(!display){free(a);return ASTER_ENOMEM;}
    lv_tick_set_cb(tick);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,a,NULL,bytes,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    perf_reset();
    lv_display_add_event_cb(display,perf_event,LV_EVENT_RENDER_START,NULL);
    lv_display_add_event_cb(display,perf_event,LV_EVENT_RENDER_READY,NULL);
#ifndef ASTER_IDF_HOST_TEST
    (void)aster_idf_ppa_fill_init();
#endif
    pointer=lv_indev_create();
    if(!pointer){lv_display_delete(display);display=NULL;free(a);return ASTER_ENOMEM;}
    lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(pointer,read_pointer);
    aster_font_sd_init();return 0;
}
void aster_ui_lvgl_pointer(uint32_t x,uint32_t y,int down) {pointer_x=x;pointer_y=y;pressed=down!=0;}
void aster_ui_lvgl_pump(void) {if(display)lv_timer_handler();}
void aster_ui_lvgl_request_frame(void) {if(display)lv_timer_ready(lv_display_get_refr_timer(display));}
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
void aster_idf_ui_idle(void)
{
    if(!display)return;
    perf_tick();uint64_t start=esp_timer_get_time();aster_launcher_pump_idle();
    uint32_t us=(uint32_t)(esp_timer_get_time()-start);perf.pump_us+=us;
    if(us>perf.max_pump_us)perf.max_pump_us=us;
}
void aster_idf_ui_before_app(void)
{bench_end=bench_warm=0;app_active=true;aster_launcher_hide();aster_qpanel_force_close();aster_gesture_reset();aster_launcher_app_touch_cancel();}
void aster_idf_ui_after_app(void)
{app_active=false;aster_qpanel_on_app_stop();aster_launcher_request_show();}
