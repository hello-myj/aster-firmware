"""Exercise runtime confirmation routing and the real close/confirm callbacks."""
import unittest
import re
import subprocess
import tempfile
from pathlib import Path
from test_waveshare import ROOT

def run_c(code):
    with tempfile.TemporaryDirectory() as directory:
        executable=str(Path(directory)/'test')
        result=subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
            '-Wno-unused-function','-I'+str(ROOT/'platform'),
            '-I'+str(ROOT/'platform'/'esp32s3'),'-I'+str(ROOT/'contract/include'),
            '-x','c','-','-o',executable],input=code,text=True,capture_output=True)
        if result.returncode: raise AssertionError(result.stderr)
        subprocess.run([executable],check=True,timeout=5)

def function(source, name):
    start=re.search(r'^(?:static )?(?:void|bool|int) '+re.escape(name)+r'\(',source,re.M).start()
    brace=source.index('{',start);depth=1;i=brace+1
    while depth:
        depth += (source[i]=='{')-(source[i]=='}');i+=1
    return source[start:i]+'\n'

class ReturnConfirmation(unittest.TestCase):
    def test_touch_slot_cli_validation_and_snapshot(self):
        source=(ROOT/'platform/esp32s3/hw_cmd.c').read_text()
        run_c(r'''
#include "aster_platform.h"
#include <aster/event.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct shell {int unused;};
#define HW_TOUCH_MAX_ITER 3000
#define shell_print(sh,...) ((void)(sh),(void)printf(__VA_ARGS__))
#define shell_error(sh,...) shell_print(sh,__VA_ARGS__)
#define shell_warn(sh,...) shell_print(sh,__VA_ARGS__)
static int calls,states;
static uint32_t got_slot,got_x,got_y,got_action;
static uint32_t recorded_x[32],recorded_y[32],recorded_action[32];
void aster_plat_input_touch_inject_slot(uint32_t slot,uint32_t x,uint32_t y,uint32_t action) {
 assert(calls<32);recorded_x[calls]=x;recorded_y[calls]=y;recorded_action[calls]=action;
 calls++;got_slot=slot;got_x=x;got_y=y;got_action=action;
}
void aster_plat_input_touch_inject(uint32_t x,uint32_t y,uint32_t action) {
 aster_plat_input_touch_inject_slot(0,x,y,action);
}
void aster_plat_input_state(uint32_t *points,uint32_t cap) {
 (void)cap;states++;points[2]=ASTER_INPUT_TERM;
}
void aster_plat_input_touch_debug(aster_touch_debug_t *out) {memset(out,0,sizeof(*out));}
bool aster_plat_input_touch_available(void) {return true;}
bool aster_plat_display_app_touch(uint32_t *x,uint32_t *y) {(void)x;(void)y;return true;}
static void k_msleep(uint32_t ms) {(void)ms;}
static uint64_t k_uptime_get(void) {return 0;}
''' + function(source,'cmd_touch') + r'''
int main(void) {
 const struct shell sh={0};
 char *valid[]={"touch","slot","1","down","120","240"};
 assert(cmd_touch(&sh,6,valid)==0 && calls==1);
 assert(got_slot==1 && got_x==120 && got_y==240 && got_action==ASTER_TOUCH_DOWN);
 valid[2]="2";assert(cmd_touch(&sh,6,valid)==-EINVAL && calls==1);
 valid[2]="1x";assert(cmd_touch(&sh,6,valid)==-EINVAL && calls==1);
 valid[2]="1";valid[4]="-1";assert(cmd_touch(&sh,6,valid)==-EINVAL && calls==1);
 valid[4]="120";valid[3]="oops";assert(cmd_touch(&sh,6,valid)==-EINVAL && calls==1);
 assert(cmd_touch(&sh,5,valid)==-EINVAL && calls==1);
 char *snapshot[]={"touch","state"};
 assert(cmd_touch(&sh,2,snapshot)==0 && states==1 && calls==1);
 char *up[]={"touch","up"};
 assert(cmd_touch(&sh,2,up)==0 && states==2 && calls==1);
 calls=0;
 char *drag[]={"touch","drag","300","476","100","380","8","30"};
 assert(cmd_touch(&sh,8,drag)==0 && calls==10);
 for(int i=1;i<=8;i++) {
  assert(recorded_x[i]==300-25*(uint32_t)i);
  assert(recorded_y[i]==476-12*(uint32_t)i);
  assert(recorded_action[i]==ASTER_TOUCH_MOVE);
 }
 assert(recorded_action[9]==ASTER_TOUCH_UP && recorded_x[9]==100 && recorded_y[9]==380);
 calls=0;drag[2]="100";drag[3]="380";drag[4]="300";drag[5]="476";
 assert(cmd_touch(&sh,8,drag)==0 && calls==10);
 for(int i=1;i<=8;i++) {
  assert(recorded_x[i]==100+25*(uint32_t)i);
  assert(recorded_y[i]==380+12*(uint32_t)i);
 }
 return 0;
}
''')

    def test_multitouch_cancels_pending_edge_return(self):
        source=(ROOT/'systems/zephyr/ui/launcher/flux.c').read_text()
        run_c(r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
static struct {bool down,edge;int32_t x,y;} app_drag;
static uint32_t aster_ui_lvgl_width(void) {return 480;}
''' + function(source,'aster_launcher_app_touch_cancel')
            + function(source,'aster_launcher_app_touch') + r'''
int main(void) {
 assert(!aster_launcher_app_touch(470,200,true));
 /* Second finger cancels the primary's pending edge gesture. */
 aster_launcher_app_touch_cancel();
 assert(!aster_launcher_app_touch(250,200,false));
 /* A fresh single finger keeps the established return behavior. */
 assert(!aster_launcher_app_touch(470,200,true));
 assert(aster_launcher_app_touch(350,200,false));
 return 0;
}
''')

    def test_runtime_pull_up_does_not_quit_before_confirm(self):
        s=(ROOT/'systems/zephyr/runtime/aster_runtime.c').read_text()
        lifecycle=function(s,'sync_lifecycle_pause')
        deliver=function(s,'deliver_touch_event')
        code=function(s,'ui_service')
        touch_ui=(ROOT/'platform/esp32s3/touch_ui.c').read_text()
        run_c(r'''
#include "aster_platform.h"
#include "touch_ui.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
typedef void lv_obj_t;
#define IS_ENABLED(x) (x)
#define CONFIG_ASTER_LVGL 1
#define CONFIG_ASTER_LAUNCHER_FLUX 0
#define ASTER_TOUCH_MAX 4
#define ASTER_PLAT_TOUCH_EVENT_QUEUE 16
#define ASTER_QPANEL_OVER_APP 1
#define LOG_INF(...) ((void)0)
enum {ASTER_GEST_NONE,ASTER_GEST_PANEL_UP};
typedef int aster_gest_t;
static struct aster_touch_ui_state runtime_touch_ui;
static uint32_t runtime_touch_ui_x,runtime_touch_ui_y;
typedef struct {const char *name;} rec_t;
typedef struct {
 bool quit,exit_clean,paused,lifecycle_paused,touch_suppressed;
 uint8_t touch_slots_down;
 const char *id;rec_t *rec;void *f_pause,*f_resume;
} aster_app_ctx_t;
#undef LOG_INF
#define LOG_INF(...) ((void)printf(__VA_ARGS__))
static aster_touch_evt_t queued[ASTER_PLAT_TOUCH_EVENT_QUEUE];
static int queued_n,gesture,requests,delivered,pumps,pauses,resumes;
static int order,resume_order,down_order;
static bool modal,confirmed,close_on_pump,watch_order,pause_exits;
int aster_plat_input_poll(aster_touch_evt_t *out,int cap) {
 int n=queued_n<cap?queued_n:cap;
 for(int i=0;i<n;i++)out[i]=queued[i];
 memmove(queued,queued+n,(size_t)(queued_n-n)*sizeof(queued[0]));queued_n-=n;
 return n;
}
static void queue_touch(uint32_t action,uint32_t mask) {
 assert(queued_n<ASTER_PLAT_TOUCH_EVENT_QUEUE);
 queued[queued_n++]=(aster_touch_evt_t){.x=160,.y=440,.action=action,.slot=0,.contact_mask=mask};
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
static bool aster_qpanel_exit_is_open(void) {return false;}
static uint64_t k_uptime_get(void) {return 0;}
bool aster_plat_display_app_touch(uint32_t *x,uint32_t *y) {(void)x;(void)y;return true;}
static void deliver_event(aster_app_ctx_t *ctx,const aster_event_t *ev) {
 (void)ctx;delivered++;
 if(watch_order&&ev->type==ASTER_EVENT_TOUCH&&ev->u.touch.action==ASTER_TOUCH_DOWN)down_order=++order;
}
static void drain_keys(aster_app_ctx_t *ctx) {(void)ctx;}
static bool aster_ui_fonts_advance(void) {return false;}
static void *aster_ui_lvgl_screen(void) {return 0;}
static void lv_obj_invalidate(void *p) {(void)p;}
static void aster_qpanel_tick(void) {}
static void aster_native_frame_refresh(aster_app_ctx_t *ctx){(void)ctx;}
static void aster_ui_lvgl_pump(void) {pumps++;if(close_on_pump){modal=false;close_on_pump=false;}}
static bool aster_qpanel_take_exit_confirmed(void) {bool c=confirmed;confirmed=false;return c;}
static void aster_native_frame_abort(aster_app_ctx_t *ctx) {(void)ctx;}
static int call_fn(aster_app_ctx_t *ctx,void *fn,uint32_t argc,uint32_t *argv) {
 (void)ctx;(void)argc;(void)argv;
 if(fn==(void*)1){pauses++;if(pause_exits)ctx->quit=true;return 0;}
 if(fn==(void*)2){resumes++;if(watch_order)resume_order=++order;return 0;}
 return 0;
}
''' + lifecycle + deliver + touch_ui + code + r'''
int main(void) {
 rec_t rec={"2048"};aster_app_ctx_t ctx={.id="aster.game.2048",.rec=&rec,.f_pause=(void*)1,.f_resume=(void*)2};
 aster_touch_ui_reset(&runtime_touch_ui);
 queue_touch(ASTER_TOUCH_DOWN,1);gesture=ASTER_GEST_PANEL_UP;ui_service(&ctx);
 assert(requests==1 && modal && !ctx.quit && !ctx.exit_clean && !delivered && pauses==1);
 queue_touch(ASTER_TOUCH_UP,0);ui_service(&ctx);assert(!ctx.quit && !delivered); /* modal owns input */
 modal=false;ui_service(&ctx);assert(resumes==1); /* close pairs the overlay pause */
 queue_touch(ASTER_TOUCH_DOWN,1);ui_service(&ctx);assert(!ctx.quit && delivered==1);
 queue_touch(ASTER_TOUCH_UP,0);ui_service(&ctx);assert(delivered==2);
 ctx.paused=true;queue_touch(ASTER_TOUCH_DOWN,1);gesture=ASTER_GEST_PANEL_UP;ui_service(&ctx);
 assert(requests==2 && !ctx.quit && ctx.paused && pauses==2 && resumes==1); /* still service a paused APP */
 confirmed=true;ui_service(&ctx);assert(ctx.quit && ctx.exit_clean && ctx.paused);
 assert(pumps==7 && pauses==2 && resumes==1 && !confirmed);

 /* A cancel button closes the sheet during LVGL pump. The next queued DOWN
  * must invoke resume before reaching the app, since resume clears controls. */
 modal=false;watch_order=false;aster_touch_ui_reset(&runtime_touch_ui);
 aster_app_ctx_t next={.id="aster.game.2048",.rec=&rec,.f_pause=(void*)1,.f_resume=(void*)2};
 modal=true;next.touch_suppressed=true;queue_touch(ASTER_TOUCH_DOWN,1);ui_service(&next);
 assert(next.lifecycle_paused&&pauses==3);
 queue_touch(ASTER_TOUCH_UP,0);close_on_pump=true;ui_service(&next);
 assert(!modal&&next.lifecycle_paused&&resumes==1);
 watch_order=true;queue_touch(ASTER_TOUCH_DOWN,1);ui_service(&next);
 assert(resume_order>0&&down_order>resume_order&&resumes==2&&delivered==3);

 /* A lifecycle callback that stops the app must not continue pumping the
  * invalid guest environment during the same UI service call. */
 aster_app_ctx_t stopping={.id="aster.game.2048",.rec=&rec,.paused=true,
  .f_pause=(void*)1,.f_resume=(void*)2};
 pause_exits=true;int prior_pumps=pumps;ui_service(&stopping);
 assert(stopping.quit&&pauses==4&&pumps==prior_pumps);
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
