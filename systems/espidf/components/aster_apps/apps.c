/* SPDX-License-Identifier: MIT */
#include "aster_idf_apps.h"
#include "aster_idf_runtime.h"
#include "aster_idf_ui.h"
#include "aster_app_mgr.h"
#include "aster_storage.h"
#include "aster_pkg.h"
#include "aster_setup.h"
#include "launcher.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
static QueueHandle_t queue;
static SemaphoreHandle_t state_lock;
static bool busy;
static void watchdog(void *unused)
{(void)unused;for(;;){aster_idf_check_watchdog();vTaskDelay(pdMS_TO_TICKS(20));}}
typedef struct { char id[64]; bool diagnostic; } job_t;
static void worker(void *unused)
{
    (void)unused;job_t job;
    int ui=aster_idf_ui_init(aster_idf_apps_launch);
    if(ui)ESP_LOGW("aster_apps","Desktop unavailable: %d",ui);
    for(;;) {
        if(xQueueReceive(queue,&job,pdMS_TO_TICKS(aster_launcher_idle_wait_ms()))!=pdTRUE){aster_idf_ui_idle();continue;}
        aster_idf_ui_before_app();
        int result=ASTER_ENOENT;
        if(job.diagnostic) {
            FILE *aot=fopen("/SD/app.aot","rb");
            if(aot){fclose(aot);result=aster_idf_run_queued_diagnostic("/SD/app.aot");}
#ifndef ASTER_REQUIRE_AOT
            if(!aot || (result==ASTER_EABI && !aster_idf_stop_is_requested()))
                result=aster_idf_run_queued_diagnostic("/SD/app.wasm");
#else
            if(!aot)ESP_LOGE("aster_apps","AOT diagnostic required: /SD/app.aot");
#endif
        } else {
            aster_app_mgr_files_lock();
            aster_app_rec_t rec;
            char path[256],dir[192];
            bool have=aster_app_mgr_find_snapshot(job.id,&rec);
            if(have && aster_storage_app_code_dir(dir,sizeof(dir),job.id)==0 &&
               snprintf(path,sizeof(path),"%s/%s",dir,rec.has_aot?"app.aot":"app.wasm")<(int)sizeof(path)) {
                /* Reserve the record before dropping files_lock; installation
                 * and uninstall cannot mutate this code generation while live. */
                aster_app_mgr_set_state(job.id,ASTER_APP_STATE_RUNNING,0);
                aster_app_mgr_note_start(job.id,esp_timer_get_time()/1000);
                aster_app_mgr_files_unlock();
                /* Map the service's Zephyr-style volume path to IDF VFS. */
                char actual[256];snprintf(actual,sizeof(actual),"/SD%s",path+4);
                result=aster_idf_run_installed(actual,job.id,rec.perm_mask);
#ifndef ASTER_REQUIRE_AOT
                if(rec.has_aot && result==ASTER_EABI && !aster_idf_stop_is_requested()) {
                    /* AOT failure before guest entry (including SRAM pressure)
                     * can fall back; never rerun an app after a guest failure. */
                    snprintf(actual,sizeof(actual),"/SD%s/app.wasm",dir+4);
                    ESP_LOGW("aster_apps","AOT unavailable for %s; retry portable WASM",job.id);
                    result=aster_idf_run_installed(actual,job.id,rec.perm_mask);
                }
#else
                if(result==ASTER_EABI)ESP_LOGE("aster_apps","AOT required for %s; WASM fallback disabled",job.id);
#endif
                aster_app_mgr_set_state(job.id,result?ASTER_APP_STATE_CRASHED:ASTER_APP_STATE_STOPPED,result);
            } else aster_app_mgr_files_unlock();
        }
        ESP_LOGI("aster_apps","APP %s finished, result=%d",job.diagnostic?"diagnostic":job.id,result);
        bool installed=aster_app_mgr_finish_install_progress(job.id,!result && !aster_idf_stop_is_requested(),aster_idf_ui_install_progress,NULL);
        aster_idf_ui_install_end();
        aster_idf_ui_after_app();
        char next[64];
        if(installed && !result && !aster_idf_stop_is_requested()) {
            aster_idf_clear_stop();
            if(xQueueSend(queue,&job,0)==pdTRUE)continue;
        }
        if(!result && !aster_idf_stop_is_requested() && aster_idf_take_handoff(next)) {
            /* Retain the single-worker reservation until the successor starts. */
            memset(&job,0,sizeof(job));snprintf(job.id,sizeof(job.id),"%s",next);
            aster_idf_clear_stop();
            if(xQueueSend(queue,&job,0)==pdTRUE)continue;
        }
        xSemaphoreTake(state_lock,portMAX_DELAY);busy=false;xSemaphoreGive(state_lock);
    }
}
static int launch(const char *id,bool diagnostic)
{
    if(!queue || !state_lock)return ASTER_ESTATE;
    if(!diagnostic && (!id || !*id || strlen(id)>=64))return ASTER_EINVAL;
    xSemaphoreTake(state_lock,portMAX_DELAY);
    if(busy){xSemaphoreGive(state_lock);return ASTER_EBUSY;}
    job_t job={.diagnostic=diagnostic};if(id)strcpy(job.id,id);
    aster_idf_clear_stop();busy=true;
    bool sent=xQueueSend(queue,&job,0)==pdTRUE;
    if(!sent)busy=false;
    xSemaphoreGive(state_lock);return sent?0:ASTER_EBUSY;
}
int aster_idf_apps_launch(const char *id) { return launch(id,false); }
int aster_idf_apps_start(void)
{
    if(queue)return 0;
    state_lock=xSemaphoreCreateMutex();queue=xQueueCreate(1,sizeof(job_t));
    if(!state_lock || !queue) {
        if(queue)vQueueDelete(queue);
        if(state_lock)vSemaphoreDelete(state_lock);
        queue=NULL;state_lock=NULL;return ASTER_ENOMEM;
    }
    if(aster_storage_mounted())aster_app_mgr_init();
    TaskHandle_t monitor=NULL;
    if(xTaskCreate(watchdog,"aster_vm_guard",3072,NULL,5,&monitor)!=pdPASS) {
        vQueueDelete(queue);vSemaphoreDelete(state_lock);queue=NULL;state_lock=NULL;return ASTER_ENOMEM;
    }
#if CONFIG_ASTER_P4_DUAL_CORE_RENDER
    if(xTaskCreatePinnedToCore(worker,"aster_vm",24576,NULL,4,NULL,1)!=pdPASS) {
#else
    if(xTaskCreate(worker,"aster_vm",24576,NULL,4,NULL)!=pdPASS) {
#endif
        vTaskDelete(monitor);
        vQueueDelete(queue);vSemaphoreDelete(state_lock);
        queue=NULL;state_lock=NULL;return ASTER_ENOMEM;
    }
#if CONFIG_ASTER_P4_AUTORUN_DIAGNOSTIC
    FILE *file=fopen("/SD/app.aot","rb");
#ifndef ASTER_REQUIRE_AOT
    if(!file)file=fopen("/SD/app.wasm","rb");
#endif
    if(file){fclose(file);return launch(NULL,true);}
#endif
    return 0;
}
int aster_idf_apps_command(int argc,char **argv)
{
    if(argc>=2 && !strcmp(argv[1],"sd")) {
        if(argc==4 && !strcmp(argv[2],"format") && !strcmp(argv[3],"--confirm")) {
            if(!state_lock)return ASTER_ESTATE;
            xSemaphoreTake(state_lock,portMAX_DELAY);
            int rc=busy?ASTER_EBUSY:aster_setup_request_format();
            if(!rc)busy=true; /* Reserve the UI/VM owner until the reset. */
            xSemaphoreGive(state_lock);
            if(!rc)puts("SD format queued: all card contents will be erased; device will reboot.");
            return rc;
        }
        puts("Erase the mounted SD card: aster sd format --confirm (stop all APPs first)");
        return ASTER_EINVAL;
    }
    if(argc>=2 && !strcmp(argv[1],"ui"))return aster_idf_ui_command(argc,argv);
    if(argc<3 || strcmp(argv[1],"app"))return ASTER_EINVAL;
    if(argc==3 && !strcmp(argv[2],"diagnostic"))return launch(NULL,true);
    if(!strcmp(argv[2],"list") && argc==3) {
        aster_app_rec_t rec;
        for(int i=0;aster_app_mgr_snapshot(i,&rec);i++)
            printf("%s | %s | state=%u | %s\n",rec.id,rec.name,(unsigned)rec.state,rec.version);
        return 0;
    }
    if(!strcmp(argv[2],"stop") && argc==3) {
        if(!state_lock)return ASTER_ESTATE;
        xSemaphoreTake(state_lock,portMAX_DELAY);aster_idf_request_stop();xSemaphoreGive(state_lock);return 0;
    }
    if(argc==3 && (!strcmp(argv[2],"pause") || !strcmp(argv[2],"resume")))
        return aster_idf_request_pause(!strcmp(argv[2],"pause"));
    if(argc!=4)return ASTER_EINVAL;
    if(!strcmp(argv[2],"run") || !strcmp(argv[2],"start")) {
        int result=aster_idf_apps_launch(argv[3]);
        if(!result)printf("started %s\n",argv[3]);
        return result;
    }
    if(!strcmp(argv[2],"install")) {
        int result=aster_app_mgr_install(argv[3]);
        if(result)printf("install failed: %d\n",result);else puts("installed OK");
        return result;
    }
    if(!strcmp(argv[2],"uninstall"))return aster_app_mgr_uninstall(argv[3]);
    return ASTER_EINVAL;
}
