/* SPDX-License-Identifier: MIT */
#include <aster/aster.h>
#include <wasm_export.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#ifndef ASTER_IDF_HOST_TEST
#include "clock.h"
#endif
static int32_t get_time(wasm_exec_env_t env,uint32_t off,uint32_t size)
{
    wasm_module_inst_t inst=wasm_runtime_get_module_inst(env);uint64_t end;
    if(size<sizeof(aster_time_info_t) || !off ||
       !wasm_runtime_get_app_addr_range(inst,off,NULL,&end) || (uint64_t)off+sizeof(aster_time_info_t)>end)return ASTER_EINVAL;
    struct timeval tv;if(gettimeofday(&tv,NULL))return ASTER_EIO;
    struct tm local;if(!localtime_r(&tv.tv_sec,&local))return ASTER_EIO;
    aster_time_info_t out={.size=sizeof(out),.version=1,.unix_ms=(uint64_t)tv.tv_sec*1000+tv.tv_usec/1000,
        .year=local.tm_year+1900,.month=local.tm_mon+1,.day=local.tm_mday,.hour=local.tm_hour,
        .minute=local.tm_min,.second=local.tm_sec,.weekday=local.tm_wday};
#ifndef ASTER_IDF_HOST_TEST
    if(aster_idf_clock_synced())out.flags|=ASTER_TIME_FLAG_SYNCED;
#endif
    memcpy(wasm_runtime_addr_app_to_native(inst,off),&out,sizeof(out));return 0;
}
static NativeSymbol symbols[]={{"aster_time_get",(void *)get_time,"(ii)i",NULL}};
NativeSymbol *aster_idf_clock_symbols(uint32_t *count) {*count=1;return symbols;}
