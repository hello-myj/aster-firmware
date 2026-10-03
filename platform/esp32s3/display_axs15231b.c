/* SPDX-License-Identifier: MIT
 * Zephyr display device for Waveshare AXS15231B QSPI glass.
 * Commands use single-lane 02 00 <cmd> 00; RAM data use 32 00 2c 00
 * followed by four-lane RGB565 MSB-first pixels. SPI2 is panel-exclusive.
 */
#define DT_DRV_COMPAT waveshare_axs15231b
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <string.h>
LOG_MODULE_REGISTER(axs15231b, LOG_LEVEL_INF);
#include "axs15231b_init.h"
struct axs_config {
 struct spi_dt_spec spi;
 uint16_t width, height;
};
struct axs_data {
 struct k_mutex lock;
 struct spi_config single, quad;
 uint8_t *frame;
 const uint8_t *borrowed; /* Persistent UI frame; ordinary writes always copy. */
 uint16_t next_row;
};
static int axs_write_bytes(const struct device *dev, const struct spi_config *cfg,
                          const void *buf, size_t size)
{
 const struct axs_config *c=dev->config;
 struct spi_buf b={.buf=(void *)buf,.len=size};
 struct spi_buf_set set={.buffers=&b,.count=1};
 return spi_write(c->spi.bus,cfg,&set);
}
static int axs_header(const struct device *dev, uint8_t opcode, uint8_t cmd)
{
 struct axs_data *d=dev->data;
 uint8_t header[4]={opcode,0,cmd,0};
 return axs_write_bytes(dev,&d->single,header,sizeof(header));
}
static void axs_end(const struct device *dev)
{
 const struct axs_config *c=dev->config;
 /* SPI_HOLD_ON_CS preserves CS across header, quad payload and DMA chunks.
  * End explicitly, including all error paths; do not leave the glass selected. */
 gpio_pin_set_dt(&c->spi.config.cs.gpio,0);
}
static int axs_cmd(const struct device *dev,uint8_t cmd,const void *data,size_t size)
{
 struct axs_data *d=dev->data;
 int err=axs_header(dev,0x02,cmd);
 if(!err && size) err=axs_write_bytes(dev,&d->single,data,size);
 axs_end(dev);
 return err;
}
static int axs_blanking(const struct device *dev,bool blank)
{
 struct axs_data *d=dev->data;
 k_mutex_lock(&d->lock,K_FOREVER);
 int err=axs_cmd(dev,blank?0x28:0x29,NULL,0);
 k_mutex_unlock(&d->lock);
 return err;
}
static int axs_on(const struct device *dev) { return axs_blanking(dev,false); }
static int axs_off(const struct device *dev) { return axs_blanking(dev,true); }
static int axs_write_frame(const struct device *dev,const uint16_t x,const uint16_t y,
                     const struct display_buffer_descriptor *desc,const void *buffer,bool persistent)
{
 const struct axs_config *c=dev->config;
 struct axs_data *d=dev->data;
 if(!desc || !buffer || !desc->width || !desc->height || desc->pitch<desc->width ||
    x+desc->width>c->width || y+desc->height>c->height ||
    desc->buf_size<((size_t)(desc->height-1)*desc->pitch+desc->width)*2u) return -EINVAL;
 k_mutex_lock(&d->lock,K_FOREVER);
 /* QSPI glass does not support arbitrary row windows. RAMWR restarts
  * at the top; RAMWRC continues the previous stream (Espressif driver).
  * Keep a wire-order shadow for partial/out-of-order rectangles, and
  * retain the fast continuation path for consecutive full-width bands. */
 bool full=x==0 && y==0 && desc->width==c->width && desc->height==c->height && desc->pitch==c->width;
 if(persistent && !full) {k_mutex_unlock(&d->lock);return -EINVAL;}
 if(persistent) d->borrowed=buffer;
 else {
  if(d->borrowed && !full) memcpy(d->frame,d->borrowed,(size_t)c->width*c->height*2u);
  d->borrowed=NULL;
 if(x==0 && desc->width==c->width && desc->pitch==c->width)
  memcpy(d->frame+(size_t)y*c->width*2u,buffer,(size_t)desc->width*desc->height*2u);
 else for(uint16_t row=0;row<desc->height;row++)
  memcpy(d->frame+((size_t)(y+row)*c->width+x)*2u,
         (const uint8_t *)buffer+(size_t)row*desc->pitch*2u,(size_t)desc->width*2u);
 }
 bool band=x==0 && desc->width==c->width && (y==0 || y==d->next_row);
 bool continuation=band && y!=0;
 uint8_t window[4];
 sys_put_be16(0,window);sys_put_be16(c->width-1,window+2);
 int err=continuation?0:axs_cmd(dev,0x2a,window,4);
 if(!err) err=axs_header(dev,0x32,continuation?0x3c:0x2c);
 size_t offset=band?(size_t)y*c->width*2u:0;
 size_t size=(size_t)c->width*(band?desc->height:c->height)*2u;
 if(!err) err=axs_write_bytes(dev,&d->quad,(persistent?(const uint8_t *)buffer:d->frame)+offset,size);
 d->next_row=err?0:(band?y+desc->height:c->height);
 axs_end(dev);
 k_mutex_unlock(&d->lock);
 return err;
}
static int axs_write(const struct device *dev,const uint16_t x,const uint16_t y,
                     const struct display_buffer_descriptor *desc,const void *buffer)
{
 return axs_write_frame(dev,x,y,desc,buffer,false);
}
/* Internal UI-only contract: buffer remains allocated for the UI lifetime;
 * callers serialize drawing and screenshot capture, and wait before reuse.
 * Never used for guest memory or short-lived staging/band buffers. */
int aster_axs15231b_write_persistent(const struct device *dev,
                     const struct display_buffer_descriptor *desc,const void *buffer)
{
 return axs_write_frame(dev,0,0,desc,buffer,true);
}
static void axs_capabilities(const struct device *dev,struct display_capabilities *cap)
{
 const struct axs_config *c=dev->config;
 memset(cap,0,sizeof(*cap));
 cap->x_resolution=c->width;cap->y_resolution=c->height;
 cap->supported_pixel_formats=PIXEL_FORMAT_RGB_565;
 cap->current_pixel_format=PIXEL_FORMAT_RGB_565;
 cap->current_orientation=DISPLAY_ORIENTATION_NORMAL;
}
static void *axs_framebuffer(const struct device *dev)
{
 struct axs_data *d=dev->data;
 return (void *)(d->borrowed?d->borrowed:d->frame);
}
static int axs_format(const struct device *dev,const enum display_pixel_format fmt)
{
 ARG_UNUSED(dev);return fmt==PIXEL_FORMAT_RGB_565?0:-ENOTSUP;
}
static int axs_orientation(const struct device *dev,const enum display_orientation orient)
{
 ARG_UNUSED(dev);
 /* Retain native window; Aster performs pixel/touch rotation for landscape. */
 return orient==DISPLAY_ORIENTATION_NORMAL?0:-ENOTSUP;
}
static int axs_init(const struct device *dev)
{
 const struct axs_config *c=dev->config;
 struct axs_data *d=dev->data;
 if(!spi_is_ready_dt(&c->spi)) return -ENODEV;
 k_mutex_init(&d->lock);
 d->frame=shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL,64,
                                         (size_t)c->width*c->height*2u);
 if(!d->frame) return -ENOMEM;
 memset(d->frame,0,(size_t)c->width*c->height*2u);
 d->single=c->spi.config;
 d->quad=d->single;d->quad.operation|=SPI_LINES_QUAD;
 int err=axs_cmd(dev,0x01,NULL,0);
 if(err) return err;
 k_msleep(150);
 for(size_t i=0;i<ARRAY_SIZE(axs_init_cmds);i++) {
  const struct axs_init_cmd *cmd=&axs_init_cmds[i];
  err=axs_cmd(dev,cmd->cmd,cmd->data,cmd->len);
  if(err) return err;
  if(cmd->delay_ms) k_msleep(cmd->delay_ms);
 }
 uint8_t rgb565=0x55;
 err=axs_cmd(dev,0x3a,&rgb565,1);
 if(!err) err=axs_cmd(dev,0x29,NULL,0);
 if(!err) LOG_INF("AXS15231B %ux%u QSPI %u MHz",c->width,c->height,c->spi.config.frequency/1000000);
 return err;
}
static DEVICE_API(display,axs_api)={
 .blanking_on=axs_off,.blanking_off=axs_on,.write=axs_write,
 .get_capabilities=axs_capabilities,.set_pixel_format=axs_format,
 .get_framebuffer=axs_framebuffer,
 .set_orientation=axs_orientation,
};
#define AXS_DEFINE(i) \
 static const struct axs_config axs_config_##i={ \
  .spi=SPI_DT_SPEC_INST_GET(i,SPI_OP_MODE_MASTER|SPI_WORD_SET(8)|SPI_HOLD_ON_CS), \
  .width=DT_INST_PROP(i,width),.height=DT_INST_PROP(i,height), \
 }; \
 static struct axs_data axs_data_##i; \
 DEVICE_DT_INST_DEFINE(i,axs_init,NULL,&axs_data_##i,&axs_config_##i, \
                       POST_KERNEL,CONFIG_APPLICATION_INIT_PRIORITY,&axs_api);
DT_INST_FOREACH_STATUS_OKAY(AXS_DEFINE)
