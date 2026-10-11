"""Check full UI frame ownership for ST7789 and AXS using actual platform code."""
import unittest
from test_waveshare import ROOT, run_c


class UIFrame(unittest.TestCase):
    def test_persistent_ui_records_st_frame_without_copy_and_keeps_axs_path(self):
        source=(ROOT/'platform/esp32s3/display.c').read_text()
        a=source.index('int aster_plat_display_write_persistent_ui(')
        implementation=source[a:source.index('int aster_plat_display_write_at_ex',a)]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ASTER_EINVAL -22
#define ASTER_OK 0
#define ASTER_EIO -5
#define CONFIG_ASTER_DISP_ASYNC 1
#define CONFIG_ASTER_PANEL_AXS15231B axs
#define IS_ENABLED(x) (x)
#define ASTER_DISP_SHARED_SLOT 0
static int axs,s_rot;
static bool s_glass_ui;
static const void *disp_dev=(void *)1;
struct display_buffer_descriptor {uint16_t width,height,pitch;size_t buf_size;};
struct aster_disp_job {const uint8_t *buf;struct display_buffer_descriptor desc;uint32_t px,py;bool persistent_ui;};
static struct aster_disp_job posted;
static const uint8_t *record;
static int submits;
static void panel_geom(uint32_t *w,uint32_t *h) {*w=240;*h=320;}
static void glass_note_full(const uint8_t *b,uint32_t w,uint32_t h,uint32_t pitch,bool alias_ok) {
 assert(w==240 && h==320 && pitch==240 && alias_ok);
 if(!axs) record=b;
}
static void disp_slot_post(int slot,const struct aster_disp_job *job) {
 assert(slot==0);posted=*job;submits++;
}
''' +implementation+r'''
int main(void) {
 static uint8_t frame[240*320*2];
 assert(aster_plat_display_write_persistent_ui(frame,240,320,240)==0);
 assert(record==frame && posted.buf==frame && !posted.persistent_ui && s_glass_ui);
 assert(posted.desc.buf_size==sizeof(frame) && posted.desc.pitch==240);
 s_rot=90;assert(aster_plat_display_write_persistent_ui(frame,240,320,240)==-22);
 s_rot=0;assert(aster_plat_display_write_persistent_ui(frame,240,320,241)==-22);
 assert(aster_plat_display_write_persistent_ui(NULL,240,320,240)==-22);
 assert(submits==1);
 axs=1;assert(aster_plat_display_write_persistent_ui(frame,240,320,240)==0);
 assert(posted.persistent_ui && submits==2 && !s_glass_ui);
 return 0;
}
''')
