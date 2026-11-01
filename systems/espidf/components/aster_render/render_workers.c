/* SPDX-License-Identifier: MIT */
#include "render_workers.h"
#include "sdkconfig.h"
#include <stdio.h>
#if CONFIG_ASTER_P4_DUAL_CORE_RENDER
#include "flux_draw.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
static QueueHandle_t queue;
static SemaphoreHandle_t done;
typedef struct {aster_flux_rows_fn fn;void *context;int first,last;uint32_t elapsed;int core;} job_t;
static uint32_t jobs,main_us,helper_us,wait_us;
static int owner_core=-1,helper_core=-1;
static void worker(void *unused)
{
    (void)unused;job_t *job;
    for(;;)if(xQueueReceive(queue,&job,portMAX_DELAY)==pdTRUE) {
        int64_t start=esp_timer_get_time();job->core=xPortGetCoreID();
        job->fn(job->context,job->first,job->last);
        job->elapsed=esp_timer_get_time()-start;xSemaphoreGive(done);
    }
}
static void execute(aster_flux_rows_fn fn,void *context,int first,int middle,int last)
{
    /* Single UI owner; source and stack job live until the join below. */
    job_t job={.fn=fn,.context=context,.first=first,.last=middle-1};job_t *pointer=&job;
    if(xPortGetCoreID()!=1 || xQueueSend(queue,&pointer,0)!=pdTRUE){fn(context,first,last);return;}
    int64_t start=esp_timer_get_time();fn(context,middle,last);main_us+=esp_timer_get_time()-start;
    start=esp_timer_get_time();xSemaphoreTake(done,portMAX_DELAY);wait_us+=esp_timer_get_time()-start;
    helper_us+=job.elapsed;owner_core=xPortGetCoreID();helper_core=job.core;jobs++;
}
void aster_idf_render_workers_init(void)
{
    if(queue)return;
    queue=xQueueCreate(1,sizeof(job_t *));done=xSemaphoreCreateBinary();
    if(!queue || !done || xTaskCreatePinnedToCore(worker,"aster_pixels0",6144,NULL,4,NULL,0)!=pdPASS) {
        if(queue)vQueueDelete(queue);
        if(done)vSemaphoreDelete(done);
        queue=NULL;done=NULL;
        ESP_LOGW("aster_render","Dual-core renderer unavailable; serial pixels retained");return;
    }
    aster_flux_set_parallel(execute);ESP_LOGI("aster_render","Dual-core pixels enabled: LVGL owner CPU1, row helper CPU0");
}
void aster_idf_render_workers_reset(void) {jobs=main_us=helper_us=wait_us=0;}
void aster_idf_render_workers_report(void)
{printf("UI SMP jobs=%lu owner_core=%d helper_core=%d owner_us=%lu helper_us=%lu join_wait_us=%lu\n",(unsigned long)jobs,owner_core,helper_core,(unsigned long)main_us,(unsigned long)helper_us,(unsigned long)wait_us);}
#else
void aster_idf_render_workers_init(void) {}
void aster_idf_render_workers_reset(void) {}
void aster_idf_render_workers_report(void) {}
#endif
