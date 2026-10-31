/* SPDX-License-Identifier: MIT */
#include "timers.h"
#include <aster/aster.h>
#include "esp_timer.h"
#include <string.h>
static struct {uint32_t handle,period,user;uint64_t next;bool armed,repeat;} timers[8];
static uint32_t serial;
static int32_t create(wasm_exec_env_t env,uint32_t ms,uint32_t repeat,uint32_t user,uint32_t out)
{
    if(!ms || ms>86400000u || repeat>1 || !out)return ASTER_EINVAL;
    wasm_module_inst_t instance=wasm_runtime_get_module_inst(env);uint64_t a,b;
    if(!wasm_runtime_get_app_addr_range(instance,out,&a,&b) || out<a || out>b || b-out<4)return ASTER_EINVAL;
    int index=-1;for(int i=0;i<8;i++)if(!timers[i].handle){index=i;break;}
    if(index<0)return ASTER_ENOSPC;
    serial=(serial&0x00ffffffu)+1;if(serial>0x00ffffffu)serial=1;
    uint32_t handle=ASTER_HTYPE_TIMER|serial;
    timers[index].handle=handle;timers[index].period=ms;timers[index].user=user;
    timers[index].repeat=repeat!=0;timers[index].armed=true;
    timers[index].next=(uint64_t)esp_timer_get_time()/1000+ms;
    memcpy(wasm_runtime_addr_app_to_native(instance,out),&handle,4);return 0;
}
static int32_t destroy(wasm_exec_env_t env,uint32_t handle)
{
    (void)env;
    for(unsigned i=0;i<8;i++)if(handle && timers[i].handle==handle) {
        memset(timers+i,0,sizeof(timers[i]));return 0;
    }
    return ASTER_EBADF;
}
bool aster_idf_next_timer(aster_event_t *event)
{
    uint64_t now=(uint64_t)esp_timer_get_time()/1000;
    for(unsigned i=0;i<8;i++)if(timers[i].armed && now>=timers[i].next) {
        if(timers[i].repeat)timers[i].next+=((now-timers[i].next)/timers[i].period+1)*timers[i].period;
        else timers[i].armed=false;
        memset(event,0,sizeof(*event));event->size=sizeof(*event);event->version=1;
        event->type=ASTER_EVENT_TIMER;event->timestamp_ms=now;
        event->u.timer.id=timers[i].handle;event->u.timer.user=timers[i].user;return true;
    }
    return false;
}
void aster_idf_timers_cleanup(void) {memset(timers,0,sizeof(timers));}
static NativeSymbol symbols[]={
 {"aster_timer_create",(void *)create,"(iiii)i",NULL},
 {"aster_timer_destroy",(void *)destroy,"(i)i",NULL},
};
NativeSymbol *aster_idf_timer_symbols(uint32_t *count) {*count=2;return symbols;}
