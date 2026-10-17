"""The actual LVGL bridge must retain short press/release across timer ticks."""
import unittest
from test_waveshare import ROOT, run_c


class LVGLPointer(unittest.TestCase):
    def test_edges_moves_wrap_and_bounded_overflow(self):
        source = (ROOT/'systems/zephyr/ui/ui_lvgl.c').read_text()
        state = source[source.index('static int32_t s_tx, s_ty;'):source.index('static int s_ready;')]
        read = source[source.index('static void pointer_read_cb('):source.index('int aster_ui_lvgl_init(')]
        write = source[source.index('void aster_ui_lvgl_pointer('):source.index('void aster_ui_lvgl_request_frame(')]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define ARG_UNUSED(x) (void)(x)
#define LV_INDEV_STATE_PRESSED 1
#define LV_INDEV_STATE_RELEASED 0
typedef int lv_indev_t;
typedef struct {struct {int32_t x,y;} point; int state; bool continue_reading;} lv_indev_data_t;
''' + state + read + write + r'''
int main(void) {
 lv_indev_data_t d={0};
 pointer_read_cb(0,&d);assert(!d.state&&!d.continue_reading);
 /* Both edges arrive before one LVGL timer pass. */
 aster_ui_lvgl_pointer(10,20,1);aster_ui_lvgl_pointer(11,21,0);
 pointer_read_cb(0,&d);assert(d.state==1&&d.point.x==10&&d.continue_reading);
 pointer_read_cb(0,&d);assert(d.state==0&&d.point.x==11&&!d.continue_reading);
 pointer_read_cb(0,&d);assert(d.state==0&&d.point.y==21&&!d.continue_reading);
 for(unsigned k=0;k<100;k++) {
  aster_ui_lvgl_pointer(1,2,1);
  for(unsigned i=0;i<100;i++)aster_ui_lvgl_pointer(i,i+1,1);
  assert(s_pointer_count==1);aster_ui_lvgl_pointer(100,101,0);
  pointer_read_cb(0,&d);assert(d.state==1&&d.point.x==99&&d.continue_reading);
  pointer_read_cb(0,&d);assert(d.state==0&&!d.continue_reading);
 }
 /* Artificial overload stays bounded and ends released. */
 for(unsigned i=0;i<100;i++)aster_ui_lvgl_pointer(i,i,(i%2)==0);
 assert(s_pointer_count==POINTER_QUEUE_SIZE);
 unsigned n=0;do {pointer_read_cb(0,&d);n++;}while(d.continue_reading);
 assert(n==POINTER_QUEUE_SIZE&&d.state==0&&d.point.x==99);
 return 0;
}
''')
