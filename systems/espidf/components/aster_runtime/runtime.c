/* SPDX-License-Identifier: MIT
 * Experimental IDF application runtime, sharing Aster device services.
 * Hardware behavior is unverified; no SDK implementation sources are linked.
 */
#include <aster/aster.h>
#include <wasm_export.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include "aster_idf_runtime.h"
#include "graphics.h"
#include "timers.h"
#ifdef ESP_PLATFORM
#include "aster_pkg.h"
#include "aster_sig.h"
#include "aster_app_mgr.h"
#include "aster_perms.h"
#include "files.h"
#include "aster_storage.h"
#ifndef ASTER_IDF_HOST_TEST
#include "wifi_service.h"
#include "aster_idf_audio.h"
#endif
bool aster_sysinfo_feature_by_name(const char *name)
{
    if (!strcmp(name,"display")) return aster_idf_feature(ASTER_FEATURE_DISPLAY);
    if (!strcmp(name,"touch")) return aster_idf_feature(ASTER_FEATURE_TOUCH);
    if (!strcmp(name,"framebuffer")) return aster_idf_feature(ASTER_FEATURE_FRAMEBUFFER);
    if (!strcmp(name,"filesystem") || !strcmp(name,"storage.app")) return aster_storage_mounted();
#ifndef ASTER_IDF_HOST_TEST
    if(!strcmp(name,"wifi"))return aster_idf_wifi_available();
    if(!strcmp(name,"audio-in") || !strcmp(name,"audio-out") || !strcmp(name,"audio-stream") ||
       !strcmp(name,"audio-effects") || !strcmp(name,"audio-effects-memory"))return aster_idf_audio_available();
    if(!strcmp(name,"rtc") || !strcmp(name,"psram") || !strcmp(name,"network") ||
       !strcmp(name,"http") || !strcmp(name,"tls") || !strcmp(name,"mqtt") || !strcmp(name,"websocket"))return true;
#endif
    return false;
}
#endif
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void *vm_malloc(unsigned int size)
{ return heap_caps_aligned_alloc(8,size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void *vm_realloc(void *ptr, unsigned int size)
{
    if(!size){free(ptr);return NULL;}
    void *next=vm_malloc(size);
    if(next && ptr) {
        size_t previous=heap_caps_get_allocated_size(ptr);
        memcpy(next,ptr,previous<size?previous:size);free(ptr);
    }
    return next;
}

static int exited, exit_code;
static aster_idf_ui_hooks_t ui_hooks;
void aster_idf_set_ui_hooks(const aster_idf_ui_hooks_t *hooks) {ui_hooks=hooks?*hooks:(aster_idf_ui_hooks_t){0};}
bool aster_idf_app_glass_frozen(void) {return ui_hooks.holds_glass && ui_hooks.holds_glass();}
void aster_idf_exit_current(int code) {exited=1;exit_code=code;}
static atomic_bool stop_requested;
static atomic_bool pause_requested;
static atomic_int stop_reason;
static atomic_uint callback_deadline;
static wasm_module_inst_t active_instance;
#ifdef ESP_PLATFORM
#include <zephyr/kernel.h>
static K_MUTEX_DEFINE(instance_guard);
static void instance_lock(void) { k_mutex_lock(&instance_guard,K_FOREVER); }
static void instance_unlock(void) { k_mutex_unlock(&instance_guard); }
#else
#include <pthread.h>
static pthread_mutex_t instance_guard=PTHREAD_MUTEX_INITIALIZER;
static void instance_lock(void) { pthread_mutex_lock(&instance_guard); }
static void instance_unlock(void) { pthread_mutex_unlock(&instance_guard); }
#endif

bool aster_idf_is_running(void) { instance_lock();bool value=active_instance!=NULL;instance_unlock();return value; }
void aster_idf_clear_stop(void) { atomic_store(&stop_reason,0);atomic_store(&pause_requested,false);atomic_store(&stop_requested,false); }
int aster_idf_request_pause(bool pause)
{
    instance_lock();bool running=active_instance!=NULL && !atomic_load(&stop_requested);
    if(running)atomic_store(&pause_requested,pause);
    instance_unlock();return running?0:ASTER_ESTATE;
}
bool aster_idf_stop_is_requested(void) {return atomic_load(&stop_requested);}
void aster_idf_check_watchdog(void)
{
    instance_lock();
    uint32_t deadline=atomic_load(&callback_deadline);
    if(active_instance && deadline && (int32_t)((uint32_t)(esp_timer_get_time()/1000)-deadline)>=0) {
        atomic_store(&stop_reason,ASTER_ETIMEDOUT);atomic_store(&stop_requested,true);
#if defined(ESP_PLATFORM) && !defined(ASTER_IDF_HOST_TEST)
        aster_idf_audio_cancel_record(true);
#endif
        wasm_runtime_terminate(active_instance);
    }
    instance_unlock();
}
void aster_idf_request_stop(void)
{
    atomic_store(&stop_requested,true);
#if defined(ESP_PLATFORM) && !defined(ASTER_IDF_HOST_TEST)
    aster_idf_audio_cancel_record(true);
#endif
    instance_lock();
    if(active_instance)wasm_runtime_terminate(active_instance);
    instance_unlock();
}
static uint32_t fps = 60;

static uint32_t now_ms(wasm_exec_env_t env)
{
    (void)env;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t abi_version(wasm_exec_env_t env)
{
    (void)env;
    return ASTER_ABI_VERSION;
}

static int32_t has_feature(wasm_exec_env_t env, uint32_t bit)
{
    (void)env;
#ifdef ESP_PLATFORM
    if(bit==ASTER_FEATURE_FILESYSTEM)return aster_storage_mounted();
    if(bit==ASTER_FEATURE_PSRAM)return true;
#ifndef ASTER_IDF_HOST_TEST
    if(bit==ASTER_FEATURE_WIFI)return aster_idf_wifi_available();
    if(bit==ASTER_FEATURE_AUDIO_INPUT || bit==ASTER_FEATURE_AUDIO_OUTPUT || bit==ASTER_FEATURE_AUDIO_STREAM ||
       bit==ASTER_FEATURE_AUDIO_EFFECTS || bit==ASTER_FEATURE_AUDIO_EFFECTS_MEMORY)return aster_idf_audio_available();
    if(bit==ASTER_FEATURE_RTC || bit==ASTER_FEATURE_NETWORK || bit==ASTER_FEATURE_HTTP ||
       bit==ASTER_FEATURE_TLS || bit==ASTER_FEATURE_MQTT || bit==ASTER_FEATURE_WEBSOCKET)return true;
#endif
#endif
    return aster_idf_feature(bit);
}
bool aster_sysinfo_has_feature(uint32_t bit) {return has_feature(NULL,bit)!=0;}
static int32_t system_info(wasm_exec_env_t env,uint32_t off,uint32_t size)
{
    wasm_module_inst_t inst=wasm_runtime_get_module_inst(env);
    if(size<sizeof(aster_system_info_t) || !off ||
       !wasm_runtime_validate_app_addr(inst,off,sizeof(aster_system_info_t)))return ASTER_EINVAL;
    uint32_t header[2];memcpy(header,wasm_runtime_addr_app_to_native(inst,off),sizeof(header));
    if(header[0]<sizeof(aster_system_info_t) || header[1]!=1)return ASTER_EINVAL;
    aster_system_info_t info;aster_idf_system_info(&info);
    info.size=sizeof(info);info.version=1;info.abi_version=ASTER_ABI_VERSION;
    for(unsigned bit=0;bit<32;bit++)if(has_feature(env,bit))info.feature_flags|=UINT64_C(1)<<bit;
    memcpy(wasm_runtime_addr_app_to_native(inst,off),&info,sizeof(info));return 0;
}

static int32_t display_count(wasm_exec_env_t env)
{
    (void)env;
    return aster_idf_display_count();
}

static void app_exit(wasm_exec_env_t env, int32_t code)
{
    (void)env;
    exited = 1;
    exit_code = code;
}

static int32_t set_framerate(wasm_exec_env_t env, uint32_t rate)
{
    (void)env;
    if (rate == 0 || rate > 120) return ASTER_EINVAL;
    fps = rate;
    return ASTER_OK;
}

static void app_log(wasm_exec_env_t env, uint32_t level, uint32_t offset, uint32_t size)
{
    (void)level;
    wasm_module_inst_t instance = wasm_runtime_get_module_inst(env);
    if (!wasm_runtime_validate_app_addr(instance, offset, size)) return;
    fwrite(wasm_runtime_addr_app_to_native(instance, offset), 1, size, stdout);
    fflush(stdout);
}

static NativeSymbol symbols[] = {
    {"aster_system_get_abi_version", (void *)abi_version, "()i", NULL},
    {"aster_system_get_info", (void *)system_info, "(ii)i", NULL},
    {"aster_system_has_feature", (void *)has_feature, "(i)i", NULL},
    {"aster_log", (void *)app_log, "(iii)", NULL},
    {"aster_time_now_ms", (void *)now_ms, "()i", NULL},
    {"aster_app_exit", (void *)app_exit, "(i)", NULL},
    {"aster_app_set_framerate", (void *)set_framerate, "(i)i", NULL},
    {"aster_display_count", (void *)display_count, "()i", NULL},
};

static int call(wasm_module_inst_t instance, wasm_exec_env_t env,
                const char *name, uint32_t count, uint32_t *args)
{
    wasm_function_inst_t function = wasm_runtime_lookup_function(instance, name);
    if (!function) {
        fprintf(stderr, "missing lifecycle export: %s\n", name);
        return 0;
    }
#ifndef CONFIG_ASTER_APP_WATCHDOG_MS
#define CONFIG_ASTER_APP_WATCHDOG_MS 2000
#endif
    uint32_t deadline=(uint32_t)(esp_timer_get_time()/1000)+CONFIG_ASTER_APP_WATCHDOG_MS;
    atomic_store(&callback_deadline,deadline?deadline:1);
    bool success=wasm_runtime_call_wasm(env,function,count,args);
    atomic_store(&callback_deadline,0);
    if (!success) {
        fprintf(stderr, "%s: %s\n", name, wasm_runtime_get_exception(instance));
        return 0;
    }
    return 1;
}

static int run_wasm(const char *path,const char *id,uint32_t perms,unsigned frame_limit)
{
#ifndef ESP_PLATFORM
    (void)id;(void)perms;
#endif
#if defined(ESP_PLATFORM) && !defined(ASTER_IDF_HOST_TEST)
    if(!atomic_load(&stop_requested))aster_idf_audio_cancel_record(false);
#endif
    exited = 0; exit_code = 0; fps = 60;
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return 2; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 2; }
    long size = ftell(file);
    if (size < 8 || size > 16 * 1024 * 1024) { fclose(file); return 2; }
    rewind(file);
    uint8_t *bytes = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        fclose(file); free(bytes); return 2;
    }
    fclose(file);
#ifdef ESP_PLATFORM
    extern void aster_idf_render_init(void);
    aster_idf_render_init();
#endif
    uint8_t *wasm_bytes=bytes;
    uint32_t wasm_size=(uint32_t)size;
#ifdef ESP_PLATFORM
    char identity[64];snprintf(identity,sizeof(identity),"%s",id?id:"__diagnostic");
    if (size>=4 && memcmp(bytes,"ASTP",4)==0) {
        aster_manifest_t manifest;
        const uint8_t *code=NULL;size_t length=0;
        int status=aster_pkg_parse(bytes,(size_t)size,&manifest,&code,&length,NULL,NULL,NULL);
        if(status==ASTER_OK)status=aster_sig_check(bytes,(size_t)size);
        if (status!=ASTER_OK) {
            fprintf(stderr,"package rejected: %d\n",status ? status : ASTER_ENOTSUP);
            free(bytes);return 2;
        }
        if(id && strcmp(id,manifest.id)) {free(bytes);return 2;}
        for(const char *p=manifest.id;*p;p++)
            if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'||*p=='.')) {free(bytes);return 2;}
        if(manifest.id[0]=='.' || strstr(manifest.id,"..")) {free(bytes);return 2;}
        snprintf(identity,sizeof(identity),"%s",manifest.id);
        perms=aster_perm_mask(manifest.permissions,manifest.permissions_n);
        wasm_bytes=(uint8_t *)code;wasm_size=(uint32_t)length;
    }
#endif
#ifdef ASTER_REQUIRE_AOT
    if(wasm_size<4 || memcmp(wasm_bytes,"\0aot",4)) {
        fprintf(stderr,"runtime: AOT required; portable WASM execution is disabled\n");
        free(bytes);return ASTER_EABI;
    }
#endif
#ifdef ESP_PLATFORM
    aster_idf_files_context(identity,perms);
    extern void aster_idf_reset_handoff(void);
    aster_idf_reset_handoff();
#endif
    RuntimeInitArgs init = {0};
    init.mem_alloc_type = Alloc_With_Allocator;
    init.mem_alloc_option.allocator.malloc_func = vm_malloc;
    init.mem_alloc_option.allocator.realloc_func = vm_realloc;
    init.mem_alloc_option.allocator.free_func = free;
    init.native_module_name = "env";
    init.native_symbols = symbols;
    init.n_native_symbols = sizeof(symbols) / sizeof(symbols[0]);
    if (!wasm_runtime_full_init(&init)) { free(bytes); return 2; }
    uint32_t extra_count;
    NativeSymbol *extra = aster_idf_graphics_symbols(&extra_count);
    if (!wasm_runtime_register_natives("env", extra, extra_count)) {
        wasm_runtime_destroy(); free(bytes); return 2;
    }
#ifdef ESP_PLATFORM
    extra=aster_idf_file_symbols(&extra_count);
    if(!wasm_runtime_register_natives("env",extra,extra_count)) {wasm_runtime_destroy();free(bytes);return 2;}
    extern NativeSymbol *aster_idf_app_symbols(uint32_t *);
    extra=aster_idf_app_symbols(&extra_count);
    if(!wasm_runtime_register_natives("env",extra,extra_count)) {wasm_runtime_destroy();free(bytes);return 2;}
    extern NativeSymbol *aster_idf_image_symbols(uint32_t *);
    extra=aster_idf_image_symbols(&extra_count);
    if(!wasm_runtime_register_natives("env",extra,extra_count)) {wasm_runtime_destroy();free(bytes);return 2;}
#ifndef ASTER_IDF_HOST_TEST
    extern NativeSymbol *aster_idf_wifi_symbols(uint32_t *);
    extra=aster_idf_wifi_symbols(&extra_count);
    if(!wasm_runtime_register_natives("env",extra,extra_count)) {wasm_runtime_destroy();free(bytes);return 2;}
    extern NativeSymbol *aster_idf_network_symbols(uint32_t *);
    extra=aster_idf_network_symbols(&extra_count);
    if(!wasm_runtime_register_natives("env",extra,extra_count)) {wasm_runtime_destroy();free(bytes);return 2;}
    extern NativeSymbol *aster_idf_audio_symbols(uint32_t *);
    extra=aster_idf_audio_symbols(&extra_count);
    if(!wasm_runtime_register_natives("env",extra,extra_count)) {wasm_runtime_destroy();free(bytes);return 2;}
#endif
#endif
    extra=aster_idf_timer_symbols(&extra_count);
    if(!wasm_runtime_register_natives("env",extra,extra_count)) {wasm_runtime_destroy();free(bytes);return 2;}
    extern NativeSymbol *aster_idf_clock_symbols(uint32_t *);
    extra=aster_idf_clock_symbols(&extra_count);
    if(!wasm_runtime_register_natives("env",extra,extra_count)) {wasm_runtime_destroy();free(bytes);return 2;}
    extern NativeSymbol *aster_idf_device_symbols(uint32_t *);
    extra=aster_idf_device_symbols(&extra_count);
    if(!wasm_runtime_register_natives("env",extra,extra_count)) {wasm_runtime_destroy();free(bytes);return 2;}
    char error[256] = {0};
    bool aot_image=wasm_size>=4 && !memcmp(wasm_bytes,"\0aot",4);
    int result = aot_image?ASTER_EABI:1;
    InstantiationArgs options={.default_stack_size=65536,.host_managed_heap_size=1024*1024,.max_memory_pages=128};
#ifdef ESP_PLATFORM
    aster_app_rec_t record;
    if(id && aster_app_mgr_find_snapshot(id,&record)) {
        if(record.mem_stack)options.default_stack_size=record.mem_stack>512*1024?512*1024:record.mem_stack;
        /* An ordinary 480x480 RGB565 surface alone needs 450 KiB. The
         * optional panel-canvas hint can fall back to guest memory, so reserve
         * up to 1 MiB even for packages using the old 256 KiB heap default.
         * The package's total linear-memory ceiling still wins below. */
        if(record.mem_heap>1024*1024)options.host_managed_heap_size=record.mem_heap>2*1024*1024?2*1024*1024:record.mem_heap;
        if(record.mem_max) {
            if(record.mem_max<65536 || record.mem_max>16*1024*1024) {fprintf(stderr,"invalid application memory budget\n");goto init_failed;}
            options.max_memory_pages=record.mem_max/65536;
        }
        uint32_t budget=options.max_memory_pages*65536;
        uint32_t initial=record.mem_initial?record.mem_initial:65536;
        if(initial>budget){fprintf(stderr,"application initial memory exceeds budget\n");goto init_failed;}
        if(options.host_managed_heap_size>budget-initial)options.host_managed_heap_size=budget-initial;
    }
#endif
    extern bool aster_idf_aot_interruptible(const uint8_t *,size_t);
    bool profile_ok=!aot_image || aster_idf_aot_interruptible(wasm_bytes,wasm_size);
    if(!profile_ok)snprintf(error,sizeof(error),"AOT requires the bounded interruptible profile; rebuild with tools/p4_aot.py");
    wasm_module_t module = profile_ok?wasm_runtime_load(wasm_bytes, wasm_size, error, sizeof(error)):NULL;
    wasm_module_inst_t instance = module ? wasm_runtime_instantiate_ex(module,&options,error,sizeof(error)) : NULL;
    wasm_exec_env_t env = instance ? wasm_runtime_create_exec_env(instance, 65536) : NULL;
    if (!env) { fprintf(stderr, "runtime: %s\n", error); goto cleanup; }
    printf("runtime: %s ready: %s\n",aot_image?"AOT":"WASM",path);
    result=1; /* Guest entry below: failures must not restart through fallback. */
    instance_lock();active_instance=instance;instance_unlock();
    if(atomic_load(&stop_requested))goto cleanup;
    uint32_t args[1] = {0};
    if (!call(instance, env, "aster_app_init", 0, args)) goto cleanup;
    if(args[0]!=ASTER_OK) {
        fprintf(stderr,"aster_app_init rejected launch: %d\n",(int)(int32_t)args[0]);
        result=exited?exit_code:1;goto cleanup;
    }
    if (!call(instance, env, "aster_app_start", 0, args)) goto cleanup;
    void *native = NULL;
    uint32_t event_offset = (uint32_t)wasm_runtime_module_malloc(instance, sizeof(aster_event_t), &native);
    if (!event_offset) goto cleanup;
    bool paused=false;
    int64_t next_frame_us=esp_timer_get_time();
    uint32_t paced_fps=fps, frame_fraction=0;
    for (unsigned frame = 0; (!frame_limit || frame < frame_limit) && !exited && !atomic_load(&stop_requested);) {
        bool want_pause=atomic_load(&pause_requested);
        if(want_pause!=paused) {
            const char *callback=want_pause?"aster_app_pause":"aster_app_resume";
            if(wasm_runtime_lookup_function(instance,callback) && !call(instance,env,callback,0,args))goto cleanup;
            paused=want_pause;aster_idf_graphics_reset_touch();
#ifdef ESP_PLATFORM
            if(id)aster_app_mgr_set_state(id,paused?ASTER_APP_STATE_PAUSED:ASTER_APP_STATE_RUNNING,0);
#endif
        }
        if(paused) {
            aster_event_t input;
            if(aster_idf_touch_event(&input) && ui_hooks.touch)ui_hooks.touch(&input);
            if(ui_hooks.tick)ui_hooks.tick();
            vTaskDelay(pdMS_TO_TICKS(10));next_frame_us=esp_timer_get_time();frame_fraction=0;continue;
        }
        aster_event_t *event = wasm_runtime_addr_app_to_native(instance, event_offset);
        memset(event, 0, sizeof(*event));
        event->size = sizeof(*event);
        event->version = 1;
        for(unsigned input_count=0;input_count<ASTER_IDF_MAX_TOUCH;input_count++) {
            if(!aster_idf_touch_event(event))break;
            event->timestamp_ms = (uint64_t)esp_timer_get_time()/1000;
            args[0] = event_offset;
            bool consumed=ui_hooks.touch && ui_hooks.touch(event);
            if(!consumed)aster_idf_map_touch(event);
            if(!consumed && !call(instance, env, "aster_app_event", 1, args)) goto cleanup;
            if (exited) break;
            /* Guest code may have grown/replaced its linear memory. */
            event = wasm_runtime_addr_app_to_native(instance, event_offset);
            memset(event, 0, sizeof(*event));
            event->size=sizeof(*event); event->version=1;
            if(!aster_idf_touch_pending())break;
        }
        if(ui_hooks.tick)ui_hooks.tick();
        aster_event_t timer_event;
        for(unsigned due=0;due<8 && !exited;due++) {
            if(!aster_idf_next_timer(&timer_event))break;
            event=wasm_runtime_addr_app_to_native(instance,event_offset);
            memcpy(event,&timer_event,sizeof(timer_event));args[0]=event_offset;
            if(!call(instance,env,"aster_app_event",1,args))goto cleanup;
        }
        if(exited)break;
#if defined(ESP_PLATFORM) && !defined(ASTER_IDF_HOST_TEST)
        extern bool aster_idf_network_event(aster_event_t *);
        for(unsigned due=0;due<8 && !exited;due++) {
            aster_event_t next;if(!aster_idf_network_event(&next))break;
            event=wasm_runtime_addr_app_to_native(instance,event_offset);
            memcpy(event,&next,sizeof(next));args[0]=event_offset;
            if(!call(instance,env,"aster_app_event",1,args))goto cleanup;
        }
        if(exited)break;
#endif
        event=wasm_runtime_addr_app_to_native(instance,event_offset);
        memset(event,0,sizeof(*event));event->size=sizeof(*event);event->version=1;
        event->type = ASTER_EVENT_FRAME;
        event->timestamp_ms = (uint64_t)esp_timer_get_time()/1000;
        event->u.frame.frame = frame;
        args[0] = event_offset;
        if (!call(instance, env, "aster_app_event", 1, args)) goto cleanup;
        frame++;
        /* Pace the entire frame, including guest drawing and DMA submission.
         * Sleeping a full period AFTER drawing caps a 10 ms scene at ~38 FPS
         * despite its requested 60. Carry the fractional microseconds rather
         * than truncating 60 Hz to 16 ms, and drop debt after an overrun. */
        int64_t now=esp_timer_get_time();
        if(paced_fps!=fps){paced_fps=fps;next_frame_us=now;frame_fraction=0;}
        next_frame_us+=1000000/paced_fps;
        frame_fraction+=1000000%paced_fps;
        if(frame_fraction>=paced_fps){next_frame_us++;frame_fraction-=paced_fps;}
        int64_t remaining=next_frame_us-now;
        if(remaining<=0){next_frame_us=now;frame_fraction=0;}
        TickType_t ticks=remaining>0?pdMS_TO_TICKS((uint32_t)(remaining/1000)):0;
        /* Always yield for watchdog/stop handling, including overloaded APPs. */
        vTaskDelay(ticks?ticks:1);
    }
    wasm_runtime_module_free(instance, event_offset);
    if (!call(instance, env, "aster_app_stop", 0, args)) goto cleanup;
    if (!call(instance, env, "aster_app_destroy", 0, args)) goto cleanup;
    result = exited ? exit_code : atomic_load(&stop_requested) ? 0 : 124;
cleanup:
    aster_idf_timers_cleanup();
    instance_lock();active_instance=NULL;instance_unlock();
    if(atomic_load(&stop_requested))result=atomic_load(&stop_reason);
#ifdef ESP_PLATFORM
#ifndef ASTER_IDF_HOST_TEST
    extern void aster_idf_network_cleanup(void);
    aster_idf_network_cleanup();
    extern void aster_idf_audio_cleanup(void);
    aster_idf_audio_cleanup();
#endif
    aster_idf_files_cleanup();
#endif
    if (instance) aster_idf_graphics_cleanup(instance);
    if (env) wasm_runtime_destroy_exec_env(env);
    if (instance) wasm_runtime_deinstantiate(instance);
    if (module) wasm_runtime_unload(module);
    wasm_runtime_destroy();
    free(bytes);
    return result;
#ifdef ESP_PLATFORM
init_failed:
    aster_idf_files_cleanup();wasm_runtime_destroy();free(bytes);return 2;
#endif
}

int aster_idf_run_wasm(const char *path) {aster_idf_clear_stop();return run_wasm(path,NULL,0,600);}
int aster_idf_run_queued_diagnostic(const char *path) {return run_wasm(path,NULL,0,600);}
int aster_idf_run_installed(const char *path,const char *id,uint32_t permissions)
{ return run_wasm(path,id,permissions,0); }
