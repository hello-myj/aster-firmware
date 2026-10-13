"""Check actual panel framing/bounds and software orientation without hardware."""
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]


def run_c(code, timeout=5):
    with tempfile.TemporaryDirectory() as directory:
        executable=str(Path(directory)/'test')
        result=subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
            '-Wno-unused-function','-x','c','-','-o',executable],
            input=code,text=True,capture_output=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        subprocess.run([executable],check=True,timeout=timeout)


class Waveshare(unittest.TestCase):
    def test_qspi_framing_pitch_bounds_and_error_deselect(self):
        source=(ROOT/'platform/esp32s3/display_axs15231b.c').read_text()
        implementation=source[source.index('struct axs_config'):source.index('static void axs_capabilities')]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define K_FOREVER 0
struct device { const void *config; void *data; };
struct k_mutex { int unused; };
struct gpio_dt_spec { int pin; };
struct spi_config { unsigned operation; struct {struct gpio_dt_spec gpio;} cs; };
struct spi_dt_spec { const struct device *bus;struct spi_config config; };
struct spi_buf { void *buf;size_t len; };
struct spi_buf_set {struct spi_buf *buffers;size_t count;};
struct display_buffer_descriptor { uint16_t width,height,pitch;size_t buf_size;};
static unsigned calls,quad[16],length[16],fail_call;static uint8_t bytes[16][16];static int deselects,locks;
static int spi_write(const struct device *dev,const struct spi_config *cfg,const struct spi_buf_set *set) {
 (void)dev;assert(set->count==1);assert(calls<16);
 quad[calls]=cfg->operation;length[calls]=set->buffers[0].len;
 memcpy(bytes[calls],set->buffers[0].buf,length[calls]>16?16:length[calls]);calls++;
 return calls==fail_call?-EIO:0;
}
static int gpio_pin_set_dt(const struct gpio_dt_spec *pin,int active) {(void)pin;assert(active==0);deselects++;return 0;}
static void k_mutex_lock(struct k_mutex *lock,int timeout) {(void)lock;(void)timeout;assert(!locks);locks++;}
static void k_mutex_unlock(struct k_mutex *lock) {(void)lock;assert(locks==1);locks--;}
static void sys_put_be16(uint16_t v,uint8_t *b) {b[0]=v>>8;b[1]=v;}
''' + implementation + r'''
int main(void) {
 struct axs_config config={.width=320,.height=480};
 static uint8_t frame[320*480*2];
 struct axs_data state={.single={.operation=1},.quad={.operation=4},.frame=frame};
 struct device dev={.config=&config,.data=&state};
 struct display_buffer_descriptor desc={.width=2,.height=2,.pitch=2,.buf_size=8};
 const uint8_t pixels[]={0xf8,0,0x07,0xe0,0,0x1f,0xff,0xff};
 assert(axs_write(&dev,318,478,&desc,pixels)==0);
 assert(calls==4 && deselects==2 && !locks);
 assert(aster_axs15231b_last_tx_bytes(&dev)==sizeof(frame));
 uint64_t completed=aster_axs15231b_tx_bytes_total(&dev);
 assert(!memcmp(bytes[0],(uint8_t[]){2,0,0x2a,0},4));
 assert(!memcmp(bytes[1],(uint8_t[]){0,0,1,63},4));
 assert(!memcmp(bytes[2],(uint8_t[]){0x32,0,0x2c,0},4));
 assert(quad[2]==1 && quad[3]==4 && length[3]==sizeof(frame));
 assert(!memcmp(frame+(478*320+318)*2,pixels,4));
 assert(!memcmp(frame+(479*320+318)*2,pixels+4,4));
 calls=0;assert(axs_write(&dev,319,478,&desc,pixels)==-EINVAL);assert(calls==0);
 desc.buf_size=7;assert(axs_write(&dev,0,0,&desc,pixels)==-EINVAL);assert(calls==0);
 desc.buf_size=10;desc.pitch=3;
 const uint8_t pitched[]={0xf8,0,7,0xe0,0xde,0xad,0,0x1f,0xff,0xff};
 assert(axs_write(&dev,0,0,&desc,pitched)==0);assert(calls==4);
 assert(!memcmp(frame,pitched,4) && !memcmp(frame+320*2,pitched+6,4));
 assert(frame[4]==0 && frame[5]==0);
 /* Ordered full-width bands must continue at row 80, rather than
  * repeatedly restarting at row zero. An unrelated rectangle invalidates
  * the stream and is recovered from the complete shadow. */
 static uint8_t band[320*80*2];memset(band,0x5a,sizeof(band));
 desc=(struct display_buffer_descriptor){320,80,320,sizeof(band)};
 calls=0;deselects=0;
 assert(axs_write(&dev,0,0,&desc,band)==0);
 assert(calls==4 && length[3]==sizeof(band) && state.next_row==80);
 calls=0;deselects=0;
 assert(axs_write(&dev,0,80,&desc,band)==0);
 assert(calls==2 && deselects==1 && state.next_row==160);
 assert(!memcmp(bytes[0],(uint8_t[]){0x32,0,0x3c,0},4));
 calls=0;deselects=0;
 assert(axs_write(&dev,0,240,&desc,band)==0);
 assert(calls==4 && length[3]==sizeof(frame) && state.next_row==480);
 calls=0;deselects=0;fail_call=2;
 assert(axs_write(&dev,0,0,&desc,band)==-EIO);
 assert(calls==2 && deselects==2 && !locks && state.next_row==0);
 assert(aster_axs15231b_last_tx_bytes(&dev)==0);
 assert(aster_axs15231b_tx_bytes_total(&dev)>=completed);
 completed=aster_axs15231b_tx_bytes_total(&dev);
 calls=0;fail_call=4;assert(axs_write(&dev,0,0,&desc,band)==-EIO);
 assert(aster_axs15231b_last_tx_bytes(&dev)==0 && aster_axs15231b_tx_bytes_total(&dev)==completed);
 /* Persistent UI frames bypass the shadow copy; a later ordinary partial
  * write materializes it before replacing only its own rectangle. */
 static uint8_t ui[320*480*2];memset(ui,0x39,sizeof(ui));memset(frame,0xab,sizeof(frame));
 desc=(struct display_buffer_descriptor){320,480,320,sizeof(ui)};
 calls=0;fail_call=0;
 assert(aster_axs15231b_write_persistent(&dev,&desc,ui)==0);
 assert(state.borrowed==ui && frame[0]==0xab && bytes[3][0]==0x39);
 desc=(struct display_buffer_descriptor){2,2,2,sizeof(pixels)};calls=0;
 assert(aster_axs15231b_write_persistent(&dev,&desc,pixels)==-EINVAL);
 assert(!calls && state.borrowed==ui && !locks);
 assert(axs_write(&dev,4,4,&desc,pixels)==0);
 assert(state.borrowed==NULL && frame[0]==0x39 && frame[(4*320+4)*2]==0xf8);
 /* Normal full frames retain ordinary display_write buffer ownership. */
 desc=(struct display_buffer_descriptor){320,480,320,sizeof(ui)};calls=0;
 assert(axs_write(&dev,0,0,&desc,ui)==0);
 ui[0]=0x88;assert(frame[0]==0x39 && state.borrowed==NULL);
 /* A native APP front may be freed after its last send drains. Releasing
  * the borrow must preserve the last image in the driver's own shadow. */
 calls=0;assert(aster_axs15231b_write_persistent(&dev,&desc,ui)==0);
 assert(state.borrowed==ui);
 aster_axs15231b_release_persistent(&dev);
 assert(state.borrowed==NULL && !locks && !memcmp(frame,ui,sizeof(frame)));
 ui[0]=0x77;assert(frame[0]==0x88);
 calls=0;fail_call=0;
 assert(aster_axs15231b_shadow_begin(&dev,true)==frame && locks==1);
 size_t completed_bytes=0;
 assert(aster_axs15231b_shadow_present_counted(&dev,0,0,320,480,&completed_bytes)==0);
 assert(completed_bytes==sizeof(frame) && !locks);
 calls=0;fail_call=4;
 assert(aster_axs15231b_shadow_begin(&dev,true)==frame);
 assert(aster_axs15231b_shadow_present_counted(&dev,0,0,320,480,&completed_bytes)==-EIO);
 assert(completed_bytes==0 && !locks);
 return 0;
}
''')

    def test_software_landscape_returns_to_native_portrait(self):
        source=(ROOT/'platform/esp32s3/display.c').read_text()
        implementation=source[source.index('static uint8_t orientation_request'):source.index('int aster_plat_display_orientation_get')]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#define ASTER_ORIENT_REQ_NONE 0xff
#define ASTER_OK 0
#define ASTER_EINVAL -22
#define ASTER_ENOTSUP -95
#define ASTER_EIO -5
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
enum {ASTER_ORIENT_PORTRAIT,ASTER_ORIENT_LANDSCAPE};
enum display_orientation {DISPLAY_ORIENTATION_NORMAL,DISPLAY_ORIENTATION_ROTATED_90};
struct display_capabilities {uint32_t x_resolution,y_resolution;int current_orientation;};
static const void *disp_dev=(void *)1;static uint16_t s_rot;static int refusal=-ENOTSUP;
static void panel_geom(uint32_t *w,uint32_t *h) {*w=320;*h=480;}
static void display_get_capabilities(const void *dev,struct display_capabilities *cap) {
 (void)dev;*cap=(struct display_capabilities){320,480,DISPLAY_ORIENTATION_NORMAL};
}
static int display_set_orientation(const void *dev,enum display_orientation orient) {
 (void)dev;(void)orient;return refusal;
}
static void glass_forget(void) {}
static int aster_plat_display_rotation_set(int deg) {s_rot=deg;return 0;}
''' + implementation + r'''
int main(void) {
 assert(aster_plat_display_orientation_set(ASTER_ORIENT_PORTRAIT)==0 && s_rot==0);
 assert(aster_plat_display_orientation_set(ASTER_ORIENT_LANDSCAPE)==0 && s_rot==90);
 assert(aster_plat_display_orientation_set(ASTER_ORIENT_LANDSCAPE)==0 && s_rot==90);
 assert(aster_plat_display_orientation_set(ASTER_ORIENT_PORTRAIT)==0 && s_rot==0);
 refusal=-EIO;
 assert(aster_plat_display_orientation_set(ASTER_ORIENT_LANDSCAPE)==ASTER_EIO && s_rot==0);
 assert(aster_plat_display_orientation_set(17)==ASTER_EINVAL && s_rot==0);
 return 0;
}
''')

    def test_direct_psram_dma_requires_alignment_and_cache_flush(self):
        patch=(ROOT/'patches/aster_zephyr_v4.4.2_psram_dma.patch').read_text()
        added='\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
        gate=added[added.index('\t\tbool direct_psram'):added.index('\t\tif (ctx->tx_buf && !direct_psram')]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define CONFIG_SPI_ESP32_TX_PSRAM_DIRECT 1
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define EIO 5
static unsigned flushes;static int fail;
static bool esp_ptr_dma_ext_capable(const void *p) {return (uintptr_t)p>=0x3d000000 && (uintptr_t)p<0x3e000000;}
static int esp_cache_msync(void *p,size_t len,int direction) {
 assert((uintptr_t)p%64==0 && len%64==0 && direction==1);flushes++;return fail;
}
static int eligible(const void *buffer,size_t dma_len_tx) {
 struct {const void *tx_buf;} state={buffer},*ctx=&state;int err=0;
''' +gate+r'''
 return direct_psram;
}
int main(void) {
 assert(eligible((void *)0x3d000040,16384)==1 && flushes==1);
 assert(eligible((void *)0x3d000060,16384)==0);
 assert(eligible((void *)0x3d000040,16380)==0);
 assert(eligible((void *)0x3fc00040,16384)==0 && eligible(NULL,64)==0);
 assert(flushes==1);fail=1;
 assert(eligible((void *)0x3d000040,64)==-5 && flushes==2);
 return 0;
}
''')

    def test_native_snapshot_drains_and_uses_driver_shadow(self):
        source=(ROOT/'platform/esp32s3/display.c').read_text()
        getter=source[source.index('bool aster_plat_display_glass_frame'):source.index('/* State for the app coordinate space')]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
static int enabled=1;
#define CONFIG_ASTER_PANEL_AXS15231B enabled
#define IS_ENABLED(x) (x)
static const void *disp_dev=(void *)1;static uint8_t frame[320*480*2],old[2];
static const uint8_t *s_glass=old;static uint32_t s_glass_w=1,s_glass_h=1,s_glass_pitch=1;
static bool s_glass_ui;
static int drained;
static int held;
static bool glass_shadow_hold(void) {held++;return s_glass!=NULL;}
static void aster_plat_display_flush_drain(void) {drained++;}
static const void *display_get_framebuffer(const void *dev) {assert(dev && drained);return frame;}
static void panel_geom(uint32_t *w,uint32_t *h) {*w=320;*h=480;}
''' +getter+r'''
int main(void) {
 const uint8_t *buf=NULL;uint32_t w,h,pitch;
 assert(aster_plat_display_glass_frame(&buf,&w,&h,&pitch));
 assert(buf==frame && w==320 && h==480 && pitch==320 && drained==1);
 enabled=0;
 assert(aster_plat_display_glass_frame(&buf,&w,&h,&pitch));
 assert(buf==old && w==1 && h==1 && pitch==1 && drained==2);
 assert(held==1);
 s_glass_ui=true;
 assert(aster_plat_display_glass_frame(&buf,&w,&h,&pitch));
 assert(buf==old && drained==3 && held==1); /* same-worker UI alias needs no copy */
 s_glass=NULL;assert(!aster_plat_display_glass_frame(&buf,&w,&h,&pitch));
 return 0;
}
''')
