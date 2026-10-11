"""Run actual FLUX gesture code with a clock and exercise circular flicks."""
import unittest
from test_waveshare import ROOT, run_c

STUBS = ''
def implementation():
    return r'''
static aster_touch_evt_t injected[16];
static int injected_n;
void aster_plat_input_touch_inject(uint32_t x, uint32_t y, uint32_t action)
{
 if(injected_n<16) injected[injected_n++]=(aster_touch_evt_t){x,y,action};
}
int aster_plat_input_poll(aster_touch_evt_t *evt, int max)
{
 int n=injected_n<max?injected_n:max;
 for(int i=0;i<n;i++) evt[i]=injected[i];
 injected_n=0;
 return n;
}
'''

HEADER = '#include "' + str(ROOT / 'systems/zephyr/ui/launcher/flux_motion.h') + '"\n'


class Motion(unittest.TestCase):
    def test_frame_cadence_and_time_based_catchup(self):
        source=(ROOT/'systems/zephyr/ui/launcher/flux.c').read_text()
        cadence=source[source.index('#define FLUX_FRAME_MS'):source.index('\n',source.index('#define FLUX_FRAME_MS'))]
        cyclic=source[source.index('static int nearest'):source.index('static entry_t *current')]
        start=source.index(' if(animating && now-last_draw')
        advance=source[start:source.index(' if(!launching) sound_launch_started',start)]
        run_c(r'''
#include <assert.h>
''' +HEADER+cadence+r'''
static int phase,from_phase,target_phase=8000,pull,from_pull,selected[4],group,count=13;
static unsigned scroll_duration=1200;
static uint64_t animation_ms,last_draw;
static bool animating=true,coasting=true,draw_pending;
static void caption(void) {}
static void save(void) {}
''' +cyclic+r'''
static void step(uint64_t now) {
''' +advance+r'''
}
int main(void) {
 step(15);assert(!draw_pending && phase==0);
 step(16);assert(draw_pending && phase>0);
 int first=phase;draw_pending=false;last_draw=16;
 step(31);assert(!draw_pending && phase==first);
 step(32);assert(draw_pending && phase>first);
 /* A slow renderer drops stale intermediate positions, then uses elapsed
  * time rather than slowing the animation down with the frame count. */
 draw_pending=false;last_draw=32;
 step(117);assert(draw_pending);
 assert(phase==8000*flux_scroll_ease(117,1200,true)/1000);
 draw_pending=false;last_draw=117;
 step(1300);assert(draw_pending && !animating && !coasting);
 assert(phase==8000 && selected[0]==8 && !pull);
 return 0;
}
''')

    def test_velocity_deceleration_and_loop_direction(self):
        run_c('#include <assert.h>\n' + HEADER + r'''
int main(void) {
 struct flux_velocity v;
 flux_velocity_reset(&v,280,100);
 flux_velocity_sample(&v,230,150);
 assert(flux_velocity_release(&v,150)==-1000);
 flux_velocity_sample(&v,231,150); /* same timestamp: accumulate */
 assert(flux_velocity_release(&v,150)==-1000);
 assert(!flux_velocity_release(&v,251)); /* stop with finger held */
 flux_velocity_sample(&v,260,250);
 assert(v.speed>0); /* reverse direction immediately */
 struct flux_scroll_plan slow=flux_scroll_plan(1200,-60,13);
 assert(!slow.coast && slow.target==1000);
 struct flux_scroll_plan fast=flux_scroll_plan(12000,-1200,13);
 assert(fast.coast && fast.target>16000 && fast.duration>1000 && fast.duration<=1800);
 assert(fast.target-12000<=8500);
 struct flux_scroll_plan backward=flux_scroll_plan(0,1200,13);
 assert(backward.coast && backward.target<-4000);
 assert(!flux_scroll_plan(500,-1200,1).coast);
 /* Even with three apps, keep the forward travel across a full loop. */
 assert(flux_scroll_plan(2000,-1200,3).target>6000);
 int previous=0;
 for(unsigned t=0;t<=fast.duration;t++) {
  int ease=flux_scroll_ease(t,fast.duration,true);
  assert(ease>=previous && ease<=1000);previous=ease;
 }
 assert(flux_scroll_ease(0,fast.duration,true)==0);
 assert(flux_scroll_ease(fast.duration,fast.duration,true)==1000);
 assert(flux_scroll_ease(100000,fast.duration,true)==1000);
 assert(flux_scroll_ease(160,320,false)==938);
 return 0;
}
''')

    def test_actual_touch_release_stop_slow_drag_and_motion_off(self):
        source=(ROOT/'systems/zephyr/ui/launcher/flux.c').read_text()
        start=source.index('static struct {\n bool down, handle, moved')
        drag=source[start:source.index('static void show_page',start)]
        cyclic=source[source.index('static int nearest'):source.index('static entry_t *current')]
        start=source.index('static void settle(int target)\n{')
        settle=source[start:source.index('static void slider_changed',start)]
        start=source.index('static void touch(const aster_touch_evt_t *')
        touch=source[start:source.index('static void tick_clock',start)]
        run_c(r'''
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#define CLAMP(v,lo,hi) ((v)<(lo)?(lo):(v)>(hi)?(hi):(v))
#define LOG_INF(...) ((void)0)
''' + HEADER + r'''
enum {HOME,ASTER_TOUCH_DOWN=1,ASTER_TOUCH_MOVE,ASTER_TOUCH_UP,ACT_OPEN};
typedef struct {uint32_t x,y,action;} aster_touch_evt_t;
static int page=HOME,group,count=13,selected[4],phase,from_phase,target_phase,pull,from_pull,action;
static unsigned scroll_duration;
static uint64_t now,animation_ms;
static bool visible=true,modal,motion=true,animating,coasting,draw_pending,launching;
static uint64_t k_uptime_get(void) {return now;}
static bool aster_wifi_settings_is_open(void) {return false;}
static void aster_ui_lvgl_pointer(int x,int y,bool pressed) {(void)x;(void)y;(void)pressed;}
static int ribbon_hit(int x,int y) {(void)x;(void)y;return selected[group];}
static void caption(void) {}
static void save(void) {}
static void to_design(uint32_t *x, uint32_t *y) {(void)x;(void)y;}
''' + drag + cyclic + settle + touch + STUBS + implementation() + r'''
static void event(unsigned ms,unsigned y,unsigned type) {
 now=ms;aster_touch_evt_t e={155,y,type};touch(&e);
}
int main(void) {
 event(0,280,ASTER_TOUCH_DOWN);event(100,180,ASTER_TOUCH_MOVE);event(120,160,ASTER_TOUCH_UP);
 assert(animating && coasting && scroll_duration>1000 && target_phase>from_phase+2500);
 assert(selected[0]==1); /* caption follows current card rather than final target */
 phase=6500;selected[0]=7;action=0;
 event(140,200,ASTER_TOUCH_DOWN);assert(!animating && drag.stopped);
 event(160,200,ASTER_TOUCH_UP);assert(!coasting && !action && !launching);
 phase=0;animating=false;
 event(300,280,ASTER_TOUCH_DOWN);event(400,180,ASTER_TOUCH_MOVE);event(550,180,ASTER_TOUCH_UP);
 assert(!coasting && scroll_duration==FLUX_SNAP_MS); /* hold cancels fling */
 phase=0;animating=false;
 event(600,280,ASTER_TOUCH_DOWN);event(4600,180,ASTER_TOUCH_MOVE);event(4620,180,ASTER_TOUCH_UP);
 assert(!coasting && target_phase==1000); /* deliberate slow selection */
 motion=false;phase=0;animating=false;
 event(4700,280,ASTER_TOUCH_DOWN);event(4800,180,ASTER_TOUCH_MOVE);event(4820,160,ASTER_TOUCH_UP);
 assert(!animating && phase==1000 && draw_pending);
 motion=true;animating=false;action=0;
 event(5000,200,ASTER_TOUCH_DOWN);event(5020,200,ASTER_TOUCH_UP);
 assert(action==ACT_OPEN); /* normal app tap retained */
 action=0;animating=false;
 event(5100,200,ASTER_TOUCH_DOWN);event(5120,192,ASTER_TOUCH_UP);
 assert(!action && drag.moved); /* small intentional swipe cannot become a click */
 action=0;animating=false;
 event(5200,200,ASTER_TOUCH_DOWN);
 now=5240;aster_touch_evt_t diagonal={270,195,ASTER_TOUCH_MOVE};touch(&diagonal);
 now=5260;diagonal.y=140;diagonal.action=ASTER_TOUCH_UP;touch(&diagonal);
 assert(!launching && !action && drag.axis==2); /* diagonal card motion never launches */
 action=0;animating=false;
 event(5300,200,ASTER_TOUCH_DOWN);event(5700,200,ASTER_TOUCH_UP);
 assert(!action); /* a held finger is not a tap */
 action=0;animating=false;
 aster_plat_input_touch_inject(155,280,ASTER_TOUCH_DOWN);
 aster_plat_input_touch_inject(155,160,ASTER_TOUCH_MOVE);
 aster_plat_input_touch_inject(155,280,ASTER_TOUCH_MOVE);
 aster_plat_input_touch_inject(155,280,ASTER_TOUCH_UP);
 aster_touch_evt_t batch[16];int n=aster_plat_input_poll(batch,16);
 now=5800;for(int i=0;i<n;i++) touch(&batch[i]);
 assert(!action && !launching && drag.moved); /* out-and-back entirely between frames */
 action=0;animating=false;
 event(5900,425,ASTER_TOUCH_DOWN);
 now=6000;aster_touch_evt_t handle={275,425,ASTER_TOUCH_UP};touch(&handle);
 assert(launching); /* explicit bottom handle still opens */
 return 0;
}
''')
