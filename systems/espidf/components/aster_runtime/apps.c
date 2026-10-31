/* SPDX-License-Identifier: MIT */
#include "files.h"
#include "aster_idf_runtime.h"
#include "aster_app_mgr.h"
#include "aster_perms.h"
#include <aster/aster.h>
#include <string.h>
#include <stdio.h>
static char handoff[64];
static void *guest(wasm_exec_env_t env,uint32_t off,uint32_t n)
{
    wasm_module_inst_t inst=wasm_runtime_get_module_inst(env);uint64_t end;
    if(!off || !n || !wasm_runtime_get_app_addr_range(inst,off,NULL,&end) || (uint64_t)off+n>end)return NULL;
    return wasm_runtime_addr_app_to_native(inst,off);
}
static int id_string(wasm_exec_env_t env,uint32_t off,uint32_t len,char id[64])
{
    const char *p=guest(env,off,len);
    if(!p || !len || len>=64 || memchr(p,0,len))return ASTER_EINVAL;
    memcpy(id,p,len);id[len]=0;
    if(id[0]=='.' || strstr(id,".."))return ASTER_EINVAL;
    for(unsigned i=0;i<len;i++)if(!((id[i]>='a'&&id[i]<='z') || (id[i]>='A'&&id[i]<='Z') ||
        (id[i]>='0'&&id[i]<='9') || id[i]=='-' || id[i]=='_' || id[i]=='.'))return ASTER_EINVAL;
    return 0;
}
static int32_t count(wasm_exec_env_t env)
{(void)env;return aster_idf_has_permission(ASTER_PERM_APPMGR)?aster_app_mgr_count():ASTER_EPERM;}
static int32_t list(wasm_exec_env_t env,uint32_t index,uint32_t off,uint32_t size)
{
    if(!aster_idf_has_permission(ASTER_PERM_APPMGR))return ASTER_EPERM;
    if(size<8)return ASTER_EINVAL;
    size_t n=size<sizeof(aster_app_info_t)?size:sizeof(aster_app_info_t);
    void *dst=guest(env,off,n);if(!dst || index>=ASTER_APP_MAX_RECS)return ASTER_EINVAL;
    aster_app_rec_t rec;if(!aster_app_mgr_snapshot((int)index,&rec))return ASTER_EINVAL;
    aster_app_info_t info={.size=sizeof(info),.version=2,.state=rec.state,.has_aot=rec.has_aot,.has_icon=rec.has_icon};
    snprintf(info.id,sizeof(info.id),"%s",rec.id);snprintf(info.name,sizeof(info.name),"%s",rec.name);
    snprintf(info.app_version,sizeof(info.app_version),"%s",rec.version);snprintf(info.category,sizeof(info.category),"%s",rec.category);
    memcpy(dst,&info,n);return 0;
}
static int32_t start(wasm_exec_env_t env,uint32_t off,uint32_t len)
{
    if(!aster_idf_has_permission(ASTER_PERM_APPMGR))return ASTER_EPERM;
    char id[64];int r=id_string(env,off,len,id);if(r)return r;
    if(!strcmp(id,aster_idf_app_identity()))return ASTER_EINVAL;
    if(handoff[0])return ASTER_EBUSY;
    aster_app_rec_t rec;if(!aster_app_mgr_find_snapshot(id,&rec))return ASTER_ENOENT;
    if(rec.state==ASTER_APP_STATE_RUNNING || rec.state==ASTER_APP_STATE_PAUSED)return ASTER_EBUSY;
    memcpy(handoff,id,len+1);aster_idf_exit_current(0);return 0;
}
static int32_t uninstall(wasm_exec_env_t env,uint32_t off,uint32_t len)
{
    if(!aster_idf_has_permission(ASTER_PERM_APPMGR))return ASTER_EPERM;
    char id[64];int r=id_string(env,off,len,id);if(r)return r;
    if(!strcmp(id,aster_idf_app_identity()))return ASTER_EINVAL;
    return aster_app_mgr_uninstall(id);
}
bool aster_idf_take_handoff(char out[64])
{if(!handoff[0])return false;memcpy(out,handoff,64);handoff[0]=0;return true;}
void aster_idf_reset_handoff(void) {handoff[0]=0;}
static NativeSymbol symbols[]={
 {"aster_appmgr_count",(void *)count,"()i",NULL},{"aster_appmgr_list",(void *)list,"(iii)i",NULL},
 {"aster_appmgr_start",(void *)start,"(ii)i",NULL},{"aster_appmgr_uninstall",(void *)uninstall,"(ii)i",NULL},
};
NativeSymbol *aster_idf_app_symbols(uint32_t *n) {*n=sizeof(symbols)/sizeof(symbols[0]);return symbols;}
