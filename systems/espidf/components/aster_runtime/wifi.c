/* SPDX-License-Identifier: MIT */
#include "files.h"
#include "wifi_service.h"
#include "aster_perms.h"
#include <aster/aster.h>
#include <string.h>
static void *guest(wasm_exec_env_t env,uint32_t off,uint32_t len)
{
    wasm_module_inst_t inst=wasm_runtime_get_module_inst(env);uint64_t end;
    if(!off || !len || !wasm_runtime_get_app_addr_range(inst,off,NULL,&end) || (uint64_t)off+len>end)return NULL;
    return wasm_runtime_addr_app_to_native(inst,off);
}
static int gate(void)
{return !aster_idf_has_permission(ASTER_PERM_WIFI)?ASTER_EPERM:!aster_idf_wifi_available()?ASTER_ESTATE:0;}
static int32_t scan(wasm_exec_env_t env)
{(void)env;int r=gate();return r?r:aster_idf_wifi_submit(ASTER_WIFI_OP_SCAN,NULL,NULL);}
static int32_t results(wasm_exec_env_t env,uint32_t off,uint32_t cap)
{
    int r=gate();if(r)return r;
    /* Only the bounded service result set can ever be written. */
    if(!cap || cap>ASTER_WIFI_AP_MAX)return ASTER_EINVAL;
    aster_wifi_ap_info_t *p=cap?guest(env,off,cap*sizeof(*p)):NULL;
    if(cap && !p)return ASTER_EINVAL;
    return aster_idf_wifi_results(p,cap);
}
static int32_t connect(wasm_exec_env_t env,uint32_t so,uint32_t sl,uint32_t po,uint32_t pl)
{
    int r=gate();if(r)return r;
    if(!sl || sl>32 || pl>64)return ASTER_EINVAL;
    const char *s=guest(env,so,sl),*p=pl?guest(env,po,pl):NULL;
    if(!s || memchr(s,0,sl) || (pl && (!p || memchr(p,0,pl))))return ASTER_EINVAL;
    char ssid[33]={0},password[65]={0};memcpy(ssid,s,sl);if(pl)memcpy(password,p,pl);
    r=aster_idf_wifi_submit(ASTER_WIFI_OP_CONNECT,ssid,password);memset(password,0,sizeof(password));return r;
}
static int32_t disconnect(wasm_exec_env_t env)
{(void)env;int r=gate();return r?r:aster_idf_wifi_submit(ASTER_WIFI_OP_DISCONNECT,NULL,NULL);}
static int32_t reconnect(wasm_exec_env_t env)
{(void)env;int r=gate();return r?r:aster_idf_wifi_submit(ASTER_WIFI_OP_RECONNECT,NULL,NULL);}
static int32_t forget(wasm_exec_env_t env)
{(void)env;int r=gate();return r?r:aster_idf_wifi_submit(ASTER_WIFI_OP_FORGET,NULL,NULL);}
static int32_t status(wasm_exec_env_t env,uint32_t off,uint32_t size)
{
    int r=gate();if(r)return r;
    if(size<offsetof(aster_wifi_info_t,last_result)+sizeof(int32_t))return ASTER_EINVAL;
    if(size>sizeof(aster_wifi_info_t))size=sizeof(aster_wifi_info_t);
    void *p=guest(env,off,size);if(!p)return ASTER_EINVAL;
    aster_wifi_info_t info;r=aster_idf_wifi_snapshot(&info);if(!r)memcpy(p,&info,size);return r;
}
static int32_t saved(wasm_exec_env_t env,uint32_t off,uint32_t cap)
{
    int r=gate();if(r)return r;
    char ssid[33];r=aster_idf_wifi_saved(ssid);if(r)return r;
    size_t n=strlen(ssid)+1;if(cap<n)return ASTER_EMSGSIZE;
    void *p=guest(env,off,n);if(!p)return ASTER_EINVAL;memcpy(p,ssid,n);return 0;
}
static NativeSymbol symbols[]={
 {"aster_wifi_scan_start",(void *)scan,"()i",NULL},{"aster_wifi_scan_results",(void *)results,"(ii)i",NULL},
 {"aster_wifi_connect",(void *)connect,"(iiii)i",NULL},{"aster_wifi_disconnect",(void *)disconnect,"()i",NULL},
 {"aster_wifi_reconnect",(void *)reconnect,"()i",NULL},{"aster_wifi_forget",(void *)forget,"()i",NULL},
 {"aster_wifi_status",(void *)status,"(ii)i",NULL},{"aster_wifi_get_saved_ssid",(void *)saved,"(ii)i",NULL},
};
NativeSymbol *aster_idf_wifi_symbols(uint32_t *n) {*n=sizeof(symbols)/sizeof(symbols[0]);return symbols;}
