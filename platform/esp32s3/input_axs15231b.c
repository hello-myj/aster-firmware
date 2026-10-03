/* SPDX-License-Identifier: MIT
 * AXS15231B touch protocol from Waveshare official esp_bsp/bsp_touch.c.
 * Report native portrait coordinates; Aster owns orientation mapping.
 */
#define DT_DRV_COMPAT waveshare_axs15231b_touch
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/kernel.h>
struct axs_touch_config { struct i2c_dt_spec bus; uint16_t width,height; };
struct axs_touch_data { const struct device *dev; struct k_work_delayable work; bool pressed; };
static void axs_touch_poll(struct k_work *work)
{
 struct axs_touch_data *d=CONTAINER_OF(k_work_delayable_from_work(work),struct axs_touch_data,work);
 const struct axs_touch_config *c=d->dev->config;
 const uint8_t request[11]={0xb5,0xab,0xa5,0x5a,0,0,0,0x0e,0,0,0};
 uint8_t data[14];
 int err=i2c_write_read_dt(&c->bus,request,sizeof(request),data,sizeof(data));
 bool down=false;
 if(!err && data[0]!=0xff && data[1]>0 && data[1]<=2) {
  uint16_t x=((data[2]&0x0f)<<8)|data[3],y=((data[4]&0x0f)<<8)|data[5];
  /* AXS event 1 means release; 0/2 mean down/contact. */
  down=(data[2]>>6)!=1 && x<c->width && y<c->height;
  if(down) {
   input_report_abs(d->dev,INPUT_ABS_X,x,false,K_NO_WAIT);
   input_report_abs(d->dev,INPUT_ABS_Y,y,false,K_NO_WAIT);
  }
 }
 if(down || d->pressed) input_report_key(d->dev,INPUT_BTN_TOUCH,down,true,K_NO_WAIT);
 d->pressed=down;
 k_work_reschedule(&d->work,K_MSEC(20));
}
static int axs_touch_init(const struct device *dev)
{
 const struct axs_touch_config *c=dev->config;struct axs_touch_data *d=dev->data;
 if(!i2c_is_ready_dt(&c->bus)) return -ENODEV;
 d->dev=dev;k_work_init_delayable(&d->work,axs_touch_poll);
 k_work_schedule(&d->work,K_MSEC(300));
 return 0;
}
#define AXS_TOUCH_DEFINE(i) \
 static const struct axs_touch_config touch_config_##i={ \
  .bus=I2C_DT_SPEC_INST_GET(i),.width=DT_INST_PROP(i,screen_width), \
  .height=DT_INST_PROP(i,screen_height), \
 }; \
 static struct axs_touch_data touch_data_##i; \
 DEVICE_DT_INST_DEFINE(i,axs_touch_init,NULL,&touch_data_##i,&touch_config_##i, \
                       POST_KERNEL,CONFIG_INPUT_INIT_PRIORITY,NULL);
DT_INST_FOREACH_STATUS_OKAY(AXS_TOUCH_DEFINE)
