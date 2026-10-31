/* SPDX-License-Identifier: MIT */
#include "files.h"
#include "imgdec.h"
#include "aster_idf_runtime.h"
#include <aster/aster.h>
#include <string.h>
static void *guest(wasm_exec_env_t env,uint32_t offset,uint32_t size)
{
    wasm_module_inst_t instance=wasm_runtime_get_module_inst(env);uint64_t end;
    if(!offset || !size || !wasm_runtime_get_app_addr_range(instance,offset,NULL,&end) || (uint64_t)offset+size>end)return NULL;
    return wasm_runtime_addr_app_to_native(instance,offset);
}
static int32_t probe(wasm_exec_env_t env,uint32_t path,uint32_t length,uint32_t output,uint32_t size)
{
    if(size<8)return ASTER_EINVAL;
    size_t count=size<sizeof(aster_img_info_t)?size:sizeof(aster_img_info_t);
    void *destination=guest(env,output,count);if(!destination)return ASTER_EINVAL;
    char real[256];int r=aster_idf_resolve_path(env,path,length,false,real);if(r)return r;
    aster_imgdec_info_t info={0};r=aster_imgdec_probe(real,&info,480,480);
    aster_img_info_t value={.size=sizeof(value),.version=1,.fmt=info.fmt,.src_w=info.src_w,.src_h=info.src_h,
        .file_size=info.file_size,.out_w=info.out_w,.out_h=info.out_h,.flags=(info.unsupported?1u:0u)|(info.too_big?2u:0u)};
    memcpy(value.why,info.why,sizeof(value.why));value.why[sizeof(value.why)-1]=0;
    memcpy(destination,&value,count);return r;
}
static int32_t render(wasm_exec_env_t env,uint32_t path,uint32_t length,uint32_t offset,uint32_t size)
{
    if(size<sizeof(aster_img_render_args_t))return ASTER_EINVAL;
    void *source=guest(env,offset,sizeof(aster_img_render_args_t));if(!source)return ASTER_EINVAL;
    aster_img_render_args_t args;memcpy(&args,source,sizeof(args));
    if(args.size<sizeof(args) || args.version!=1 || args.reserved || args.flags&~ASTER_IMG_F_NEAREST || !args.cap_w || !args.cap_h)return ASTER_EINVAL;
    uint64_t required=(uint64_t)args.cap_w*args.cap_h*2;
    if(required>UINT32_MAX || (args.dst_span && args.dst_span<required))return ASTER_EINVAL;
    void *destination=guest(env,args.dst_off,(uint32_t)required);if(!destination)return ASTER_EINVAL;
    char real[256];int r=aster_idf_resolve_path(env,path,length,false,real);if(r)return r;
    args.dst_span=required;args.out_w=args.out_h=0;
    r=aster_imgdec_render(real,destination,args.cap_w,args.cap_h,&args.out_w,&args.out_h,(args.flags&ASTER_IMG_F_NEAREST)!=0);
    memcpy(source,&args,sizeof(args));return r;
}
static NativeSymbol symbols[]={{"aster_img_probe",(void *)probe,"(iiii)i",NULL},{"aster_img_render",(void *)render,"(iiii)i",NULL}};
NativeSymbol *aster_idf_image_symbols(uint32_t *count) {*count=2;return symbols;}
