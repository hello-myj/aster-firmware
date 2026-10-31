/* SPDX-License-Identifier: MIT */
#include "clock.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <time.h>
#include <stdlib.h>
#include <sys/time.h>
#include <aster/time.h>
#include <string.h>
#include <errno.h>
static atomic_bool synced;
static bool configured,started;
static void synchronized(struct timeval *now) {(void)now;atomic_store(&synced,true);}
bool aster_idf_clock_synced(void) {return atomic_load(&synced);}
int aster_time_read(aster_time_info_t *out)
{
    if(!out)return -EINVAL;
    struct timeval tv;struct tm local;
    if(gettimeofday(&tv,NULL) || !localtime_r(&tv.tv_sec,&local))return -EIO;
    *out=(aster_time_info_t){.size=sizeof(*out),.version=1,.unix_ms=(uint64_t)tv.tv_sec*1000+tv.tv_usec/1000,
        .flags=aster_idf_clock_synced()?ASTER_TIME_FLAG_SYNCED:0,.year=local.tm_year+1900,
        .month=local.tm_mon+1,.day=local.tm_mday,.hour=local.tm_hour,.minute=local.tm_min,
        .second=local.tm_sec,.weekday=local.tm_wday};
    return 0;
}
void aster_idf_clock_init(void)
{
    setenv("TZ","CST-8",1);tzset();
    if(time(NULL)<1704067200) {
        struct timeval initial={.tv_sec=1704067200};settimeofday(&initial,NULL);
    }
    esp_sntp_config_t config=ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start=false;config.sync_cb=synchronized;
    esp_err_t r=esp_netif_sntp_init(&config);configured=r==ESP_OK;
    if(r!=ESP_OK)ESP_LOGE("aster_time","SNTP initialization: %s",esp_err_to_name(r));
}
void aster_idf_clock_network_ready(void)
{
    /* Called only on the default event loop, after the first station IPv4. */
    if(configured && !started) {
        esp_err_t r=esp_netif_sntp_start();started=r==ESP_OK;
        if(r!=ESP_OK)ESP_LOGE("aster_time","SNTP start: %s",esp_err_to_name(r));
    }
}
