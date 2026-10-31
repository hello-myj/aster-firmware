/* SPDX-License-Identifier: MIT */
#include "files.h"
#include "aster_kv.h"
#include <aster/aster.h>
#include "aster_storage.h"
#include "aster_perms.h"
#include "aster_kv.h"
#include <string.h>
#include <errno.h>
#include <limits.h>
static char app_id[64];
static uint32_t permissions,serial;
bool aster_idf_has_permission(uint32_t bit) {return (permissions&bit)!=0;}
const char *aster_idf_app_identity(void) {return app_id;}
static struct { uint32_t handle;bool writable,readable;struct fs_file_t file; } files[8];
static struct { uint32_t handle;bool pending;struct fs_dir_t dir;struct fs_dirent entry; } dirs[4];
static void *guest(wasm_exec_env_t env,uint32_t off,uint32_t len)
{
    wasm_module_inst_t inst=wasm_runtime_get_module_inst(env);uint64_t a,b;
    if(!off || !wasm_runtime_get_app_addr_range(inst,off,&a,&b) || off<a || off>b || len>b-off)return NULL;
    return wasm_runtime_addr_app_to_native(inst,off);
}
static int error(int r)
{
    if(r>=0)return r;
    switch(-r) {
    case ENOENT:return ASTER_ENOENT;case EEXIST:return ASTER_EEXIST;
    case ENOMEM:return ASTER_ENOMEM;case ENOSPC:return ASTER_ENOSPC;
    case EINVAL:case ENAMETOOLONG:return ASTER_EINVAL;
    case EBADF:return ASTER_EBADF;case EPERM:case EACCES:return ASTER_EPERM;
    default:return ASTER_EIO;
    }
}
static int string(wasm_exec_env_t env,uint32_t off,uint32_t len,char *out,size_t cap)
{
    if(len==UINT32_MAX) {
        wasm_module_inst_t inst=wasm_runtime_get_module_inst(env);uint64_t a,b;
        const char *p=guest(env,off,1);
        if(!p || !wasm_runtime_get_app_addr_range(inst,off,&a,&b))return ASTER_EINVAL;
        size_t available=b-off;if(available>cap)available=cap;
        const char *end=memchr(p,0,available);if(!end)return ASTER_EINVAL;
        len=(uint32_t)(end-p);
    }
    if(!len || len>=cap)return ASTER_EINVAL;
    const char *p=guest(env,off,len);if(!p || memchr(p,0,len))return ASTER_EINVAL;
    memcpy(out,p,len);out[len]=0;return 0;
}
int aster_idf_resolve_path(wasm_exec_env_t env,uint32_t off,uint32_t len,bool write,char out[256])
{
    if(!(permissions&ASTER_PERM_FS))return ASTER_EPERM;
    char path[160];int r=string(env,off,len,path,sizeof(path));if(r)return r;
    if(strchr(path,'\\') || strchr(path,':'))return ASTER_EPERM;
    if(write && (aster_storage_is_music_path(path) ||
       (!strncmp(path,"/assets",7) && (!path[7] || path[7]=='/'))))return ASTER_EPERM;
    r=aster_storage_sandbox(out,256,app_id,path,permissions);return error(r);
}
void aster_idf_files_context(const char *id,uint32_t perms)
{ strncpy(app_id,id,sizeof(app_id)-1);app_id[sizeof(app_id)-1]=0;permissions=perms; }
void aster_idf_files_cleanup(void)
{
    for(unsigned i=0;i<8;i++)if(files[i].handle){fs_close(&files[i].file);memset(files+i,0,sizeof(files[i]));}
    for(unsigned i=0;i<4;i++)if(dirs[i].handle){fs_closedir(&dirs[i].dir);memset(dirs+i,0,sizeof(dirs[i]));}
    permissions=0;app_id[0]=0;
}
static uint32_t handle(bool directory)
{ if(++serial>0x00ffffffu)serial=1;return serial|(directory?ASTER_HTYPE_DIR:ASTER_HTYPE_FILE); }
static int find_file(uint32_t h)
{ if(h)for(int i=0;i<8;i++)if(files[i].handle==h)return i;return -1; }
static int find_dir(uint32_t h)
{ if(h)for(int i=0;i<4;i++)if(dirs[i].handle==h)return i;return -1; }
static int32_t open_file(wasm_exec_env_t env,uint32_t off,uint32_t len,uint32_t flags,uint32_t out)
{
    bool write=(flags&3)==ASTER_FS_O_WRONLY || (flags&3)==ASTER_FS_O_RDWR;
    if(!(flags&3) || (flags&~0x73u) || (!write && (flags&(ASTER_FS_O_TRUNC|ASTER_FS_O_APPEND))))return ASTER_EINVAL;
    if(!guest(env,out,4))return ASTER_EINVAL;
    char path[256];int r=aster_idf_resolve_path(env,off,len,write || (flags&ASTER_FS_O_CREAT),path);if(r)return r;
    int index=-1;for(int i=0;i<8;i++)if(!files[i].handle){index=i;break;}
    if(index<0)return ASTER_ENOSPC;
    int f=(flags&1)?FS_O_READ:0;if(write)f|=FS_O_WRITE;
    if(flags&ASTER_FS_O_CREAT)f|=FS_O_CREATE;
    if(flags&ASTER_FS_O_TRUNC)f|=FS_O_TRUNC;
    if(flags&ASTER_FS_O_APPEND)f|=FS_O_APPEND;
    fs_file_t_init(&files[index].file);r=fs_open(&files[index].file,path,f);if(r)return error(r);
    uint32_t h=handle(false);files[index].handle=h;files[index].writable=write;files[index].readable=(flags&1)!=0;
    memcpy(guest(env,out,4),&h,4);return 0;
}
static int32_t read_file(wasm_exec_env_t env,uint32_t h,uint32_t off,uint32_t len)
{
    int i=find_file(h);if(i<0)return ASTER_EBADF;if(!files[i].readable)return ASTER_EPERM;if(!len)return 0;
    void *p=guest(env,off,len);if(!p || len>INT_MAX)return ASTER_EINVAL;
    return error(aster_fs_read_safe(&files[i].file,p,len));
}
static int32_t write_file(wasm_exec_env_t env,uint32_t h,uint32_t off,uint32_t len)
{
    int i=find_file(h);if(i<0)return ASTER_EBADF;if(!files[i].writable)return ASTER_EPERM;if(!len)return 0;
    void *p=guest(env,off,len);if(!p || len>INT_MAX)return ASTER_EINVAL;
    return error(aster_fs_write_safe(&files[i].file,p,len));
}
static int32_t seek_file(wasm_exec_env_t env,uint32_t h,int32_t off,uint32_t whence)
{ (void)env;int i=find_file(h);if(i<0)return ASTER_EBADF;if(whence>2)return ASTER_EINVAL;return error(fs_seek(&files[i].file,off,whence)); }
static int32_t close_file(wasm_exec_env_t env,uint32_t h)
{ (void)env;int i=find_file(h);if(i<0)return ASTER_EBADF;int r=fs_close(&files[i].file);files[i].handle=0;return error(r); }
static int32_t make_dir(wasm_exec_env_t env,uint32_t off,uint32_t len)
{ char path[256];int r=aster_idf_resolve_path(env,off,len,true,path);return r?r:error(fs_mkdir(path)); }
static int32_t unlink_file(wasm_exec_env_t env,uint32_t off,uint32_t len)
{ char path[256];int r=aster_idf_resolve_path(env,off,len,true,path);return r?r:error(fs_unlink(path)); }
static int32_t stat_file(wasm_exec_env_t env,uint32_t off,uint32_t len,uint32_t out,uint32_t size)
{
    if(size<sizeof(aster_stat_t) || !guest(env,out,sizeof(aster_stat_t)))return ASTER_EINVAL;
    char path[256];int r=aster_idf_resolve_path(env,off,len,false,path);if(r)return r;
    struct fs_dirent st;r=fs_stat(path,&st);if(r)return error(r);
    aster_stat_t value={.size=sizeof(value),.version=1,.filesize=st.size,.mode=st.type==FS_DIR_ENTRY_DIR?ASTER_STAT_DIR:0};
    memcpy(guest(env,out,sizeof(value)),&value,sizeof(value));return 0;
}
static int32_t open_dir(wasm_exec_env_t env,uint32_t off,uint32_t len,uint32_t out)
{
    if(!guest(env,out,4))return ASTER_EINVAL;
    char path[256];int r=aster_idf_resolve_path(env,off,len,false,path);if(r)return r;
    int i=-1;for(int j=0;j<4;j++)if(!dirs[j].handle){i=j;break;}if(i<0)return ASTER_ENOSPC;
    fs_dir_t_init(&dirs[i].dir);r=fs_opendir(&dirs[i].dir,path);if(r)return error(r);
    uint32_t h=handle(true);dirs[i].handle=h;dirs[i].pending=false;memcpy(guest(env,out,4),&h,4);return 0;
}
static int32_t next_dir(wasm_exec_env_t env,uint32_t h,uint32_t out,uint32_t cap,uint32_t is_dir)
{
    int i=find_dir(h);if(i<0)return ASTER_EBADF;
    if(!cap || !guest(env,out,cap) || !guest(env,is_dir,4))return ASTER_EINVAL;
    struct fs_dirent *entry=&dirs[i].entry;
    while(!dirs[i].pending) {
        int r=fs_readdir(&dirs[i].dir,entry);if(r)return error(r);
        if(!entry->name[0])return 0;
        if(!strcmp(entry->name,".") || !strcmp(entry->name,"..") || !aster_fs_name_valid(entry->name))continue;
        dirs[i].pending=true;
    }
    size_t n=strlen(entry->name);if(n+1>cap)return ASTER_EMSGSIZE;
    memcpy(guest(env,out,cap),entry->name,n+1);uint32_t flag=entry->type==FS_DIR_ENTRY_DIR;
    memcpy(guest(env,is_dir,4),&flag,4);dirs[i].pending=false;return n;
}
static int32_t close_dir(wasm_exec_env_t env,uint32_t h)
{ (void)env;int i=find_dir(h);if(i<0)return ASTER_EBADF;int r=fs_closedir(&dirs[i].dir);dirs[i].handle=0;return error(r); }
static int key(wasm_exec_env_t env,uint32_t off,uint32_t len,char out[49])
{
    if(!(permissions&ASTER_PERM_KV))return ASTER_EPERM;
    int r=string(env,off,len,out,49);if(r)return r;
    if(strstr(out,"..") || strchr(out,'/') || strchr(out,'\\') || strchr(out,':'))return ASTER_EINVAL;
    return 0;
}
static int32_t kv_set(wasm_exec_env_t env,uint32_t off,uint32_t len,uint32_t value,uint32_t size)
{ char k[49];int r=key(env,off,len,k);if(r)return r;void *p=size?guest(env,value,size):NULL;if(size && !p)return ASTER_EINVAL;if(size>65536)return ASTER_EINVAL;return error(aster_kv_set(app_id,k,p,size)); }
static int32_t kv_get(wasm_exec_env_t env,uint32_t off,uint32_t len,uint32_t value,uint32_t cap)
{ char k[49];int r=key(env,off,len,k);if(r)return r;void *p=cap?guest(env,value,cap):NULL;if(cap && !p)return ASTER_EINVAL;size_t got=0;r=aster_kv_get(app_id,k,p,cap,&got);return r?error(r):(int)got; }
static int32_t kv_delete(wasm_exec_env_t env,uint32_t off,uint32_t len)
{ char k[49];int r=key(env,off,len,k);return r?r:error(aster_kv_delete(app_id,k)); }
static NativeSymbol symbols[]={
 {"aster_fs_open",(void *)open_file,"(iiii)i",NULL},{"aster_fs_read",(void *)read_file,"(iii)i",NULL},
 {"aster_fs_write",(void *)write_file,"(iii)i",NULL},{"aster_fs_seek",(void *)seek_file,"(iii)i",NULL},
 {"aster_fs_close",(void *)close_file,"(i)i",NULL},{"aster_fs_mkdir",(void *)make_dir,"(ii)i",NULL},
 {"aster_fs_unlink",(void *)unlink_file,"(ii)i",NULL},{"aster_fs_stat",(void *)stat_file,"(iiii)i",NULL},
 {"aster_fs_opendir",(void *)open_dir,"(iii)i",NULL},{"aster_fs_dir_next",(void *)next_dir,"(iiii)i",NULL},
 {"aster_fs_closedir",(void *)close_dir,"(i)i",NULL},
 {"aster_kv_set",(void *)kv_set,"(iiii)i",NULL},{"aster_kv_get",(void *)kv_get,"(iiii)i",NULL},
 {"aster_kv_delete",(void *)kv_delete,"(ii)i",NULL},
};
NativeSymbol *aster_idf_file_symbols(uint32_t *count) { *count=sizeof(symbols)/sizeof(symbols[0]);return symbols; }
