/* SPDX-License-Identifier: MIT */
#include "aster_watchdog.h"
#include "aster_app_mgr.h"
#include "aster_mem.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <string.h>
#include <stdio.h>
#if defined(CONFIG_SOC_SERIES_ESP32S3)
#include <esp_attr.h>
#include <esp_rom_sys.h>
#define WATCH_RETAIN RTC_NOINIT_ATTR
#else
#define WATCH_RETAIN __noinit
#endif
LOG_MODULE_REGISTER(aster_watch,LOG_LEVEL_INF);

static const char *const phases[]={"none","init","start","event","pause","resume","stop","destroy"};
static K_MUTEX_DEFINE(watch_lock);
static struct {char id[64];uint32_t start,limit;enum aster_callback_phase phase;bool armed;} watch;
static struct k_thread watch_thread;
/* Retained reset reason has no guest/host object pointers. */
struct aster_reset_record {uint32_t magic,phase,elapsed;char id[64];uint32_t crc;};
static WATCH_RETAIN struct aster_reset_record previous;
static uint32_t reason_crc(void)
{
 const uint8_t *p=(const uint8_t *)&previous;uint32_t crc=2166136261u;
 for(size_t i=0;i<offsetof(struct aster_reset_record,crc);i++){crc=(crc^p[i])*16777619u;}
 return crc;
}
static bool expired(uint32_t now,uint32_t start,uint32_t limit)
{return (uint32_t)(now-start)>=limit;}
static void watch_fn(void *a,void *b,void *c)
{
 (void)a;(void)b;(void)c;
 for(;;){
  k_msleep(10);k_mutex_lock(&watch_lock,K_FOREVER);
  uint32_t now=k_uptime_get_32();
  if(watch.armed&&expired(now,watch.start,watch.limit)){
   previous.magic=0x41574831u;previous.phase=watch.phase;previous.elapsed=now-watch.start;
   memcpy(previous.id,watch.id,sizeof(previous.id));previous.crc=reason_crc();
   LOG_ERR("APP callback timeout: %s phase=%s elapsed=%u; system recovery",watch.id,phases[watch.phase],previous.elapsed);
#if defined(CONFIG_SOC_SERIES_ESP32S3)
   esp_rom_software_reset_system();
#else
   sys_reboot(SYS_REBOOT_COLD);
#endif
  }
  k_mutex_unlock(&watch_lock);
 }
}
int aster_watchdog_init(void)
{
 if(!IS_ENABLED(CONFIG_ASTER_APP_WATCHDOG))return 0;
 if(previous.magic==0x41574831u&&previous.phase>0&&previous.phase<8&&previous.id[63]==0&&previous.crc==reason_crc()){
  LOG_ERR("ASTER_RECOVERED id=%s phase=%s elapsed=%u",previous.id,phases[previous.phase],previous.elapsed);
  aster_app_mgr_set_state(previous.id,ASTER_APP_STATE_CRASHED,8);
 }
 previous.magic=0;
 k_thread_stack_t *stack=aster_dram_alloc(K_KERNEL_STACK_LEN(3072));
 if(!stack)return -ENOMEM;
 k_thread_create(&watch_thread,stack,K_KERNEL_STACK_LEN(3072)-K_KERNEL_STACK_RESERVED,
                 watch_fn,NULL,NULL,NULL,K_PRIO_PREEMPT(2),0,K_NO_WAIT);
 k_thread_name_set(&watch_thread,"aster_watch");return 0;
}
void aster_watchdog_begin(const char *id,enum aster_callback_phase phase)
{
 if(!IS_ENABLED(CONFIG_ASTER_APP_WATCHDOG))return;
 k_mutex_lock(&watch_lock,K_FOREVER);snprintf(watch.id,sizeof(watch.id),"%s",id);
 watch.phase=phase;watch.start=k_uptime_get_32();
 watch.limit=(phase==ASTER_CB_INIT||phase==ASTER_CB_START)?CONFIG_ASTER_APP_INIT_TIMEOUT_MS:
             (phase==ASTER_CB_STOP||phase==ASTER_CB_DESTROY)?CONFIG_ASTER_APP_STOP_TIMEOUT_MS:CONFIG_ASTER_APP_EVENT_TIMEOUT_MS;
 watch.armed=true;k_mutex_unlock(&watch_lock);
}
int aster_watchdog_end(void)
{
 if(!IS_ENABLED(CONFIG_ASTER_APP_WATCHDOG))return 0;
 k_mutex_lock(&watch_lock,K_FOREVER);
 int late=watch.armed&&expired(k_uptime_get_32(),watch.start,watch.limit);
 watch.armed=false;k_mutex_unlock(&watch_lock);return late;
}
