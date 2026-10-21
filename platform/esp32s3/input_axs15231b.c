/* SPDX-License-Identifier: MIT
 * AXS15231B touch protocol from Waveshare official esp_bsp/bsp_touch.c.
 * Report native portrait coordinates; Aster owns orientation mapping.
 */
#define DT_DRV_COMPAT waveshare_axs15231b_touch
#include "aster_platform.h"
#include "touch_protocol.h"
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(aster_axs15231b, LOG_LEVEL_INF);

#if defined(CONFIG_ASTER_AXS15231B_RAW_DIAG)
static atomic_t replay_tick;

int aster_plat_axs_touch_diag_replay_start(void)
{
 return atomic_cas(&replay_tick,0,1)?0:-EBUSY;
}

static void diagnostic_replay(uint8_t data[14], int *err)
{
 unsigned tick=(unsigned)atomic_get(&replay_tick);
 if(!tick) return;
 /* Still perform the real bus read before overriding its report. This is
  * a driver-path fault fixture, not evidence of physical touch success. */
 *err=0;
 memset(data,0,14);
 if(tick>=4 && tick<=9) {
  memset(data,0xa3,14);
 } else if(tick<25) {
  unsigned event=(tick<=3 || (tick>=13 && tick<=16))?0:
                 tick>=21?1:2;
  data[1]=2;
  data[2]=(uint8_t)(event<<6); data[3]=160;
  data[4]=0; data[5]=200;
  data[8]=(uint8_t)(event<<6); data[9]=200;
  data[10]=0x11; data[11]=4; /* ID1, y=260 */
 }
 if(tick==1 || tick==4 || tick==10 || tick==13 || tick==21 || tick==25)
  LOG_INF("AXS replay tick=%u count=%u event=%u",tick,data[1],data[2]>>6);
 if(tick>=30) {
  atomic_set(&replay_tick,0);
  LOG_INF("AXS replay completed");
 } else {
  atomic_inc(&replay_tick);
 }
}
#endif

struct axs_touch_config { struct i2c_dt_spec bus; uint16_t width,height; };
struct axs_touch_data {
 const struct device *dev;
 struct k_work_delayable work;
 uint8_t invalid_frames;
#if defined(CONFIG_ASTER_AXS15231B_RAW_DIAG)
 uint8_t dual_frame_diag;
 uint32_t frame_signature;
 int64_t transition_log_ms;
#endif
};
static void axs_touch_poll(struct k_work *work)
{
 struct axs_touch_data *d=CONTAINER_OF(k_work_delayable_from_work(work),struct axs_touch_data,work);
 const struct axs_touch_config *c=d->dev->config;
 const uint8_t request[11]={0xb5,0xab,0xa5,0x5a,0,0,0,0x0e,0,0,0};
 uint8_t data[14];
#if defined(CONFIG_ASTER_AXS15231B_RAW_DIAG)
 memset(data,0xcd,sizeof(data));
#endif
 int err=i2c_write_read_dt(&c->bus,request,sizeof(request),data,sizeof(data));
#if defined(CONFIG_ASTER_AXS15231B_RAW_DIAG)
 diagnostic_replay(data,&err);
#endif
 if(err) {
  aster_plat_input_touch_report_error((uint64_t)k_uptime_get());
 } else {
#if defined(CONFIG_ASTER_AXS15231B_RAW_DIAG)
  /* Observe count/event/ID transitions as well as held dual contacts.
   * Include explicit UP/empty records without flooding logs with moves. */
  if(data[0]!=0xff && data[1]<=2) {
   uint32_t signature=1u+data[1]+((uint32_t)(data[2]>>6)<<4)+
    ((uint32_t)(data[4]>>4)<<6);
   if(data[1]==2) signature+=((uint32_t)(data[8]>>6)<<10)+
    ((uint32_t)(data[10]>>4)<<12);
   int64_t now=k_uptime_get();
   if(signature!=d->frame_signature && now-d->transition_log_ms>=100) {
    LOG_HEXDUMP_INF(data,sizeof(data),"AXS contact transition raw frame");
    d->frame_signature=signature;
    d->transition_log_ms=now;
   }
  }
  /* Bounded field check for the competing AXS ID descriptions. Once per
   * second during a two-contact report, retain the complete 14-byte frame
   * and the two candidate ID nibbles in the console log. */
  if(data[1]==2) {
   if(++d->dual_frame_diag>=50) {
    LOG_INF("AXS two-contact candidate IDs: %u/%u",
            (unsigned)(data[4]>>4),(unsigned)(data[10]>>4));
    LOG_HEXDUMP_INF(data,sizeof(data),"AXS two-contact raw frame");
    d->dual_frame_diag=0;
   }
  } else {
   d->dual_frame_diag=0;
  }
#endif
  struct aster_touch_frame frame;
  err=aster_axs15231b_decode(data,sizeof(data),c->width,c->height,&frame);
  if(err) {
   if(++d->invalid_frames==50) {
    LOG_WRN("repeated invalid AXS15231B touch frames (%d)",err);
#if defined(CONFIG_ASTER_AXS15231B_RAW_DIAG)
    LOG_HEXDUMP_INF(data,sizeof(data),"AXS rejected raw frame");
#endif
    d->invalid_frames=0;
   }
   aster_plat_input_touch_report_error((uint64_t)k_uptime_get());
  } else {
   d->invalid_frames=0;
   aster_plat_input_touch_report_frame(&frame);
  }
 }
 k_work_reschedule(&d->work,K_MSEC(20));
}
static int axs_touch_init(const struct device *dev)
{
 const struct axs_touch_config *c=dev->config;struct axs_touch_data *d=dev->data;
 if(!i2c_is_ready_dt(&c->bus)) { aster_plat_input_touch_set_ready(false); return -ENODEV; }
 d->dev=dev;k_work_init_delayable(&d->work,axs_touch_poll);
 aster_plat_input_touch_set_ready(true);
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
