"""A canvas writer must wait before modifying pixels still owned by a send."""
import unittest
from test_waveshare import ROOT, run_c


class CanvasOwnership(unittest.TestCase):
    def test_waits_for_selected_buffer_before_exposing_pixels(self):
        text = (ROOT/'platform/esp32s3/display.c').read_text()
        body = text[text.index('uint8_t *aster_plat_display_canvas_back(void)\n{'):text.index('int aster_plat_display_canvas_present(void)\n{')]
        for asynchronous in (0, 1):
            with self.subTest(asynchronous=asynchronous):
                run_c('#define CONFIG_ASTER_DISP_ASYNC '+str(asynchronous)+r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define IS_ENABLED(x) (x)
#define ASTER_DISP_FLIP_SLOT0 1
static uint8_t pixels[2][4]={{0xaa},{0xbb}};
static uint8_t *s_front[2]={pixels[0],pixels[1]},s_flip;
struct sem {int token;unsigned completions;};
static struct {struct sem free;} s_slots[3];
static bool ready=true;
static unsigned waits;
static bool aster_plat_display_canvas_ready(void){return ready;}
static void disp_slot_acquire(uint8_t slot){
 assert(slot==s_flip+1);waits++;
 struct sem *sem=&s_slots[slot].free;
 if(!sem->token){ /* emulate delayed DMA finishing; its old bytes must survive */
  assert(pixels[slot-1][0]==(slot==1?0xaa:0xbb));sem->completions++;sem->token=1;
 }
 sem->token--;
}
static void k_sem_give(struct sem *sem){assert(sem->token==0);sem->token++;}
''' + body + r'''
int main(void){
 ready=false;assert(!aster_plat_display_canvas_back()&&!waits);ready=true;
 uint8_t *p=aster_plat_display_canvas_back();assert(p==pixels[0]);
 if(CONFIG_ASTER_DISP_ASYNC){assert(waits==1&&s_slots[1].free.completions==1);assert(!s_slots[2].free.completions);}
 p[0]=0x11;assert(aster_plat_display_canvas_back()==p);
 if(CONFIG_ASTER_DISP_ASYNC){assert(s_slots[1].free.token==1);disp_slot_acquire(1);assert(!s_slots[1].free.token);k_sem_give(&s_slots[1].free);}
 s_flip=1;p=aster_plat_display_canvas_back();assert(p==pixels[1]);
 if(CONFIG_ASTER_DISP_ASYNC){assert(s_slots[2].free.completions==1&&s_slots[2].free.token==1);}
 p[0]=0x22;assert(pixels[0][0]==0x11);return 0;
}
''')
