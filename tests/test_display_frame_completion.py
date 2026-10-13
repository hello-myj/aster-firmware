"""A completed failed display job must reach transaction END as an error."""
import unittest
from test_waveshare import ROOT,run_c
class FrameCompletion(unittest.TestCase):
 def test_completion_error_consumed_only_by_checked_drain(self):
  source=(ROOT/'platform/esp32s3/display.c').read_text()
  plain=source[source.index('void aster_plat_display_flush_drain(void)'):source.index('/* Transaction END')]
  checked=source[source.index('int aster_plat_display_flush_drain_checked(void)'):source.index('int aster_plat_display_count(void)')]
  run_c(r'''
#include <assert.h>
#define CONFIG_ASTER_DISP_ASYNC 1
#define IS_ENABLED(x) (x)
#define K_FOREVER 0
#define ASTER_EIO -7
#define ASTER_DISP_JOB_SLOTS 3
struct slot {int result,free;};
static struct slot s_slots[3];
static void k_sem_take(int *p,int wait){(void)wait;assert(*p==1);*p=0;}
static void k_sem_give(int *p){assert(*p==0);*p=1;}
'''+plain+checked+r'''
int main(void){
 for(int i=0;i<3;i++)s_slots[i].free=1;
 s_slots[1].result=-123;
 aster_plat_display_flush_drain();assert(s_slots[1].result==-123);
 assert(aster_plat_display_flush_drain_checked()==ASTER_EIO);
 for(int i=0;i<3;i++)assert(s_slots[i].free==1&&s_slots[i].result==0);
 assert(aster_plat_display_flush_drain_checked()==0);return 0;
}
''')
