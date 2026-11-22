"""Exercise the USB sector transfer code without mounting a real card."""
import unittest

from test_waveshare import ROOT, run_c


class FakePodUSBStorage(unittest.TestCase):
    def test_partial_io_bounds_and_storage_ownership(self):
        source = (ROOT / 'systems/espidf/adapters/usb_sdmmc/usb_sdmmc.c').read_text()
        transfer = source[source.index('static bool medium_ready'):
                          source.index('bool tud_msc_start_stop_cb')]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
enum {DMA_BYTES=8192, ESP_OK=0, SCSI_SENSE_NOT_READY=2, SCSI_SENSE_ILLEGAL_REQUEST=5,
      SCSI_SENSE_MEDIUM_ERROR=3, portMAX_DELAY=1000};
typedef int esp_err_t;
typedef struct { struct { size_t sector_size, capacity; } csd; } sdmmc_card_t;
static sdmmc_card_t card = {{512, 5}};
static unsigned sense, asc, lock, writes;
static bool force_error;
static uint8_t disk[5*512], scratch[DMA_BYTES], *dma=scratch;
static int io_lock;
static atomic_bool io_enabled, ejected;
#define ESP_LOGW(...)
static void tud_msc_set_sense(uint8_t lun, int key, int code, int qualifier)
{ (void)lun; (void)qualifier; sense=key; asc=code; }
static void xSemaphoreTake(int mutex,int timeout)
{ (void)mutex; (void)timeout; assert(!lock++); }
static void xSemaphoreGive(int mutex)
{ (void)mutex; assert(lock--==1); }
static int sdmmc_read_sectors(sdmmc_card_t *c, void *p,size_t sector,size_t n)
{ assert(lock && sector+n<=c->csd.capacity); memcpy(p,disk+sector*512,n*512); return 0; }
static int sdmmc_write_sectors(sdmmc_card_t *c,const void *p,size_t sector,size_t n)
{
 assert(lock && sector+n<=c->csd.capacity);
 if (force_error) return 1;
 memcpy(disk+sector*512,p,n*512);writes++;return 0;
}
''' + transfer + r'''
int main(void)
{
 uint8_t block[1024], copy[1024];
 memset(disk,0xaa,sizeof(disk));memset(block,0x65,sizeof(block));
 assert(__wrap_tud_msc_write10_cb(0,0,0,block,sizeof(block))==-1 && !writes);
 io_enabled=true;
 assert(__wrap_tud_msc_write10_cb(0,0,510,block,sizeof(block))==sizeof(block));
 assert(disk[509]==0xaa && disk[510]==0x65 && disk[1533]==0x65 && disk[1534]==0xaa);
 assert(__wrap_tud_msc_read10_cb(0,0,510,copy,sizeof(copy))==sizeof(copy));
 assert(memcmp(block,copy,sizeof(block))==0);
 assert(__wrap_tud_msc_write10_cb(0,4,0,block,sizeof(block))==-1);
 assert(sense==SCSI_SENSE_ILLEGAL_REQUEST && asc==0x21);
 force_error=true;
 assert(__wrap_tud_msc_write10_cb(0,0,0,block,512)==-1);
 assert(sense==SCSI_SENSE_MEDIUM_ERROR);
 ejected=true;
 assert(__wrap_tud_msc_read10_cb(0,0,0,copy,512)==-1 && sense==SCSI_SENSE_NOT_READY);
 assert(!lock);
 return 0;
}
''')
