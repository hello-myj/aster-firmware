/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <aster/aster.h>
#include <wasm_export.h>
#include "files.h"
#include "aster_perms.h"
#include <string.h>
#include <limits.h>
/* Reuse the shipping ABI 4 checked bridges, including protocol ownership,
 * bounded payload copies, status/error mapping and stale-handle protection. */
typedef struct {uint64_t network_owner;} aster_app_ctx_t;
static aster_app_ctx_t context;
static aster_app_ctx_t *ctx_of(wasm_exec_env_t env) {(void)env;return &context;}
static bool aster_ctx_perm(aster_app_ctx_t *ctx,uint32_t bit) {(void)ctx;return aster_idf_has_permission(bit);}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../../zephyr/runtime/aster_network_bridge.inc"
#include "../../../zephyr/runtime/aster_http_bridge.inc"
#include "../../../zephyr/runtime/aster_mqtt_bridge.inc"
#include "../../../zephyr/runtime/aster_ws_bridge.inc"
#include "../../../zephyr/runtime/aster_net_bridge.inc"
#pragma GCC diagnostic pop
void aster_idf_network_cleanup(void) {aster_native_release_network(&context);}
bool aster_idf_network_event(aster_event_t *out)
{
    uint32_t handle,kind,state;
    if(!aster_net_event_take(context.network_owner,&handle,&kind,&state))return false;
    uint32_t public_handle=aster_net_public_handle(context.network_owner,kind,handle);
    if(!public_handle)return false;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->version=1;
    out->type=ASTER_EVENT_SYS;out->timestamp_ms=k_uptime_get();
    out->u.sys.code=ASTER_SYS_NET_STATE;out->u.sys.value=public_handle;
    out->u.data[2]=kind;out->u.data[3]=state;return true;
}
static NativeSymbol symbols[]={
 {"aster_net_info",(void *)n_net_info,"(ii)i",NULL},{"aster_net_open",(void *)n_net_open,"(ii)i",NULL},
 {"aster_net_status",(void *)n_net_status,"(iii)i",NULL},{"aster_net_io",(void *)n_net_io,"(iiii)i",NULL},
 {"aster_net_cancel",(void *)n_net_cancel,"(i)i",NULL},{"aster_net_close",(void *)n_net_close,"(i)i",NULL},
};
NativeSymbol *aster_idf_network_symbols(uint32_t *n) {*n=sizeof(symbols)/sizeof(symbols[0]);return symbols;}
