"""Real input callback/poll path must preserve edge gestures across a busy frame."""
from pathlib import Path
import unittest
from test_waveshare import run_c
ROOT=Path(__file__).resolve().parents[1]
class InputCoalescing(unittest.TestCase):
    def test_press_origin_and_release_survive_delayed_poll(self):
        source=(ROOT/'platform/esp32s3/input.c').read_text()
        state=source[source.index('static struct {'):source.index('INPUT_CALLBACK_DEFINE')]
        poll=source[source.index('int aster_plat_input_poll('):source.index('void aster_plat_input_state(')]
        inject=source[source.index('void aster_plat_input_touch_inject('):]
        # The injection function is the final definition in this translation unit.
        run_c(r'''
#include "aster_platform.h"
#include <assert.h>
#include <string.h>
#define K_MUTEX_DEFINE(n) int n
#define K_FOREVER 0
#define ARG_UNUSED(x) (void)(x)
#define INPUT_EV_ABS 1
#define INPUT_EV_KEY 2
#define INPUT_ABS_X 0
#define INPUT_ABS_Y 1
#define INPUT_BTN_TOUCH 330
struct input_event {unsigned type,code;int value;};
static void k_mutex_lock(int *p,int timeout){(void)timeout;assert(!*p);*p=1;}
static void k_mutex_unlock(int *p){assert(*p);*p=0;}
static bool available=true;
bool aster_plat_input_touch_available(void){return available;}
void aster_plat_display_touch_map(uint32_t *x,uint32_t *y){(void)x;(void)y;}
void aster_plat_display_touch_unmap(uint32_t *x,uint32_t *y){(void)x;(void)y;}
''' + state + poll + inject + r'''
static void event(unsigned type,unsigned code,int value){struct input_event e={type,code,value};input_cb(&e,NULL);}
static void point(unsigned x,unsigned y){event(INPUT_EV_ABS,INPUT_ABS_X,x);event(INPUT_EV_ABS,INPUT_ABS_Y,y);}
int main(void){
 aster_touch_evt_t e[2];
 /* Device reports press then moves before the APP reaches its next frame. */
 point(100,4);event(INPUT_EV_KEY,INPUT_BTN_TOUCH,1);point(100,180);
 assert(aster_plat_input_poll(e,2)==2);
 assert(e[0].action==ASTER_TOUCH_DOWN&&e[0].y==4);
 assert(e[1].action==ASTER_TOUCH_MOVE&&e[1].y==180);
 event(INPUT_EV_KEY,INPUT_BTN_TOUCH,0);assert(aster_plat_input_poll(e,2)==1&&e[0].action==ASTER_TOUCH_UP);
 /* A whole fast swipe arrives between frames; final movement precedes UP. */
 point(100,475);event(INPUT_EV_KEY,INPUT_BTN_TOUCH,1);point(100,260);event(INPUT_EV_KEY,INPUT_BTN_TOUCH,0);
 assert(aster_plat_input_poll(e,2)==2);
 assert(e[0].action==ASTER_TOUCH_DOWN&&e[0].y==475);
 assert(e[1].action==ASTER_TOUCH_MOVE&&e[1].y==260);
 assert(aster_plat_input_poll(e,2)==1&&e[0].action==ASTER_TOUCH_UP&&e[0].y==260);
 /* A one-event consumer must not lose pending movement or release. */
 aster_plat_input_touch_inject(90,3,ASTER_TOUCH_DOWN);
 aster_plat_input_touch_inject(90,190,ASTER_TOUCH_MOVE);
 aster_plat_input_touch_inject(90,190,ASTER_TOUCH_UP);
 assert(aster_plat_input_poll(e,1)==1&&e[0].action==ASTER_TOUCH_DOWN&&e[0].y==3);
 assert(aster_plat_input_poll(e,1)==1&&e[0].action==ASTER_TOUCH_MOVE&&e[0].y==190);
 assert(aster_plat_input_poll(e,1)==1&&e[0].action==ASTER_TOUCH_UP);
 assert(!aster_plat_input_poll(e,2));
 available=false;aster_plat_input_touch_inject(1,1,ASTER_TOUCH_DOWN);
 assert(!aster_plat_input_poll(e,2));available=true;assert(!aster_plat_input_poll(e,2));
 return 0;
}
''')
