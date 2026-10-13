"""Raw diagnostic must send real CASET/RASET patches, never a shadow fallback."""
import unittest
from test_waveshare import ROOT, run_c


class WindowDiagnostic(unittest.TestCase):
 def test_raw_rectangle_bytes_order_and_cleanup(self):
  source=(ROOT/'platform/esp32s3/display_axs15231b.c').read_text()
  start=source.index('#if defined(CONFIG_ASTER_AXS_WINDOW_TEST)')
  body=source[start:source.index('static int axs_write_frame(',start)]
  run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#define CONFIG_ASTER_AXS_WINDOW_TEST 1
#define K_FOREVER 0
struct spi_config {int op;};
struct axs_config {uint16_t width,height;};
struct axs_data {int lock;struct spi_config quad;uint8_t *frame;const uint8_t *borrowed;uint16_t next_row;size_t last_tx_bytes;uint64_t tx_bytes_total;};
struct device {const struct axs_config *config;struct axs_data *data;};
static uint8_t cmd[12],args[12][4],colors[3][2];
static int commands,payloads,headers,deselects,live,fail_at,alloc_failed;
static void sys_put_be16(uint16_t value,uint8_t *p){p[0]=value>>8;p[1]=value;}
static void *k_malloc(size_t n){if(alloc_failed)return NULL;live++;return malloc(n);}
static void k_free(void *p){assert(p);live--;free(p);}
static void k_mutex_lock(int *m,int wait){(void)wait;assert(!*m);*m=1;}
static void k_mutex_unlock(int *m){assert(*m==1);*m=0;}
static int axs_cmd(const struct device *dev,uint8_t c,const void *data,size_t n){
 assert(dev->data->lock&&n==4&&commands<12);cmd[commands]=c;memcpy(args[commands],data,4);commands++;
 return fail_at==commands?-EIO:0;
}
static int axs_header(const struct device *dev,uint8_t op,uint8_t c){assert(dev->data->lock&&op==0x32&&c==0x2c);headers++;return 0;}
static int axs_write_bytes(const struct device *dev,const struct spi_config *cfg,const void *p,size_t n){
 assert(dev->data->lock&&cfg->op==4&&n==2048&&payloads<3);memcpy(colors[payloads++],p,2);return 0;
}
static void axs_end(const struct device *dev){assert(dev->data->lock);deselects++;}
static int axs_tx_locked(const struct device *dev,uint16_t x,uint16_t y,uint16_t w,uint16_t h,const void *p){
 assert(dev->data->lock&&!x&&!y&&w==320&&h==480&&p==dev->data->frame);
 const uint8_t *b=p;assert(b[0]==0xff&&b[1]==0xff);return 0;
}
'''+body+r'''
int main(void){
 static uint8_t frame[320*480*2];struct axs_config cfg={320,480};
 struct axs_data d={.quad={4},.frame=frame,.borrowed=frame,.next_row=480};struct device dev={&cfg,&d};
 uint32_t bytes=123;assert(aster_axs15231b_window_test(&dev,&bytes)==0);
 assert(bytes==6144&&payloads==3&&headers==3&&commands==8&&deselects==3&&!live&&!d.lock&&!d.borrowed&&!d.next_row);
 for(int i=0;i<3;i++)assert(cmd[i*2]==0x2a&&cmd[i*2+1]==0x2b);
 assert(!memcmp(args[0],(uint8_t[]){0,16,0,47},4));
 assert(!memcmp(args[1],(uint8_t[]){0,16,0,47},4));
 assert(!memcmp(args[2],(uint8_t[]){0,144,0,175},4));
 assert(!memcmp(args[3],(uint8_t[]){0,224,0,255},4));
 assert(!memcmp(args[4],(uint8_t[]){1,16,1,47},4));
 assert(!memcmp(args[5],(uint8_t[]){1,176,1,207},4));
 assert(!memcmp(colors,(uint8_t[]){0xf8,0,7,0xe0,0,0x1f},6));
 assert(!memcmp(args[6],(uint8_t[]){0,0,1,63},4)&&!memcmp(args[7],(uint8_t[]){0,0,1,223},4));
 /* Raw patch colors must NOT be mirrored into the screenshot shadow. */
 assert(frame[(16*320+16)*2]==0x10&&frame[(16*320+16)*2+1]==0x82);
 commands=payloads=headers=deselects=0;fail_at=4;
 assert(aster_axs15231b_window_test(&dev,&bytes)==-EIO&&bytes==2048);
 assert(payloads==1&&commands==6&&!live&&!d.lock&&!d.next_row);
 alloc_failed=1;assert(aster_axs15231b_window_test(&dev,&bytes)==-ENOMEM&&bytes==0);
 return 0;
}
''')
