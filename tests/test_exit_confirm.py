"""Exercise runtime confirmation routing and the real close/confirm callbacks."""
import unittest
import re
from test_waveshare import ROOT, run_c

def function(source, name):
    start=re.search(r'^(?:static )?(?:void|bool|int) '+re.escape(name)+r'\(',source,re.M).start()
    brace=source.index('{',start);depth=1;i=brace+1
    while depth:
        depth += (source[i]=='{')-(source[i]=='}');i+=1
    return source[start:i]+'\n'

class ReturnConfirmation(unittest.TestCase):
    def test_runtime_pull_up_does_not_quit_before_confirm(self):
        s=(ROOT/'systems/zephyr/runtime/aster_runtime.c').read_text()
        code=function(s,'ui_service')
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef void lv_obj_t;
#define IS_ENABLED(x) (x)
#define CONFIG_ASTER_LVGL 1
#define CONFIG_ASTER_LAUNCHER_FLUX 0
#define ASTER_TOUCH_MAX 4
#define ASTER_TOUCH_UP 3
#define ASTER_EVENT_TOUCH 1
#define ASTER_QPANEL_OVER_APP 1
#define LOG_INF(...) ((void)0)
enum {ASTER_GEST_NONE,ASTER_GEST_PANEL_UP};
typedef int aster_gest_t;
typedef struct {int x,y,action;} aster_touch_evt_t;
typedef struct {const char *name;} rec_t;
typedef struct {bool quit,exit_clean,paused;const char *id;rec_t *rec;} aster_app_ctx_t;
typedef struct {int size,version,type;uint64_t timestamp_ms;struct {struct {int x,y,action;} touch;} u;} aster_event_t;
#undef LOG_INF
#define LOG_INF(...) ((void)printf(__VA_ARGS__))
static int pending,gesture,requests,delivered,pumps,restores;
static bool modal,confirmed;
static int aster_plat_input_poll(aster_touch_evt_t *out,int cap) {
 (void)cap;if(!pending)return 0;pending=0;*out=(aster_touch_evt_t){160,440,2};return 1;
}
static uint32_t aster_qpanel_allow(bool home) {(void)home;return 0;}
static int aster_ui_lvgl_height(void) {return 480;}
static int aster_gesture_feed(int x,int y,bool down,uint32_t allow,int h) {
 (void)x;(void)y;(void)down;(void)allow;(void)h;int g=gesture;gesture=0;return g;
}
static void aster_ui_lvgl_pointer(int x,int y,bool p) {(void)x;(void)y;(void)p;}
static void *aster_ui_lvgl_indev(void) {return 0;}
static void lv_indev_wait_release(void *p) {(void)p;}
static bool aster_qpanel_on_gesture(int g,int host) {(void)g;(void)host;return false;}
static bool aster_qpanel_request_exit(const char *name) {assert(!strcmp(name,"2048"));requests++;modal=true;return true;}
static void aster_gesture_reset(void) {}
static bool aster_qpanel_is_open(void) {return modal;}
static uint64_t k_uptime_get(void) {return 0;}
static bool aster_plat_display_app_touch(int *x,int *y) {(void)x;(void)y;return true;}
static void deliver_event(aster_app_ctx_t *ctx,aster_event_t *ev) {(void)ctx;(void)ev;delivered++;}
static void drain_keys(aster_app_ctx_t *ctx) {(void)ctx;}
static bool aster_ui_fonts_advance(void) {return false;}
static void *aster_ui_lvgl_screen(void) {return 0;}
static void lv_obj_invalidate(void *p) {(void)p;}
static void aster_qpanel_tick(void) {}
static void aster_ui_lvgl_pump(void) {pumps++;}
static bool aster_qpanel_take_exit_confirmed(void) {bool c=confirmed;confirmed=false;return c;}
''' + code + r'''
int main(void) {
 rec_t rec={"2048"};aster_app_ctx_t ctx={.id="aster.game.2048",.rec=&rec};
 pending=1;gesture=ASTER_GEST_PANEL_UP;ui_service(&ctx);
 assert(requests==1 && modal && !ctx.quit && !ctx.exit_clean && !delivered);
 pending=1;ui_service(&ctx);assert(!ctx.quit && !delivered); /* modal owns input */
 modal=false;restores++;pending=1;ui_service(&ctx);assert(!ctx.quit && delivered==1);
 ctx.paused=true;pending=1;gesture=ASTER_GEST_PANEL_UP;ui_service(&ctx);
 assert(requests==2 && !ctx.quit && ctx.paused); /* still service a paused APP */
 confirmed=true;ui_service(&ctx);assert(ctx.quit && ctx.exit_clean && ctx.paused);
 assert(pumps==5 && restores==1 && !confirmed);
 return 0;
}
''')

    def test_cancel_restores_glass_confirm_waits_for_runtime_and_teardown(self):
        s=(ROOT/'systems/zephyr/ui/qpanel.c').read_text()
        code='\n'.join(function(s,n) for n in ('aster_qpanel_close','aster_qpanel_dismiss','aster_qpanel_force_close','on_exit_cancel','on_exit_confirm','aster_qpanel_exit_is_open','aster_qpanel_take_exit_confirmed'))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define ARG_UNUSED(x) ((void)(x))
#define LOG_INF(...) ((void)0)
typedef int lv_event_t;
static bool s_frozen,s_exit_mode,s_exit_confirmed;
static void *s_scrim,*s_sheet;
static int restored,drained,refresh;
static void ui_teardown(void) {s_scrim=s_sheet=NULL;s_exit_mode=false;}
static void lv_refr_now(void *p) {(void)p;refresh++;}
static void aster_plat_display_flush_drain(void) {drained++;}
static void glass_put_back(void) {restored++;}
''' + code + r'''
static void open(void) {s_sheet=s_scrim=(void*)1;s_frozen=s_exit_mode=true;}
int main(void) {
 open();assert(aster_qpanel_exit_is_open());on_exit_cancel(NULL);
 assert(!s_frozen && !aster_qpanel_exit_is_open() && restored==1 && drained==1 && refresh==1);
 assert(!aster_qpanel_take_exit_confirmed());
 open();on_exit_confirm(NULL);assert(s_frozen && aster_qpanel_exit_is_open());
 assert(aster_qpanel_take_exit_confirmed() && !aster_qpanel_take_exit_confirmed());
 aster_qpanel_force_close();assert(!s_frozen && !s_sheet && restored==1);
 open();on_exit_confirm(NULL);aster_qpanel_force_close();assert(!aster_qpanel_take_exit_confirmed());
 return 0;
}
''')
