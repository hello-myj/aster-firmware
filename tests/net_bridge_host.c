/* SPDX-License-Identifier: MIT: actual service + actual native bridge. */
#define ASTER_NET_HOST_TEST
#include "../systems/zephyr/services/aster_net.c"
#include <aster/aster.h>
#include "../systems/zephyr/services/aster_perms.h"
#define CONFIG_ASTER_NET 1
#define IS_ENABLED(n) (n)
#define zsock_inet_ntop inet_ntop
static unsigned slept;
static void k_sleep(int n) { slept += n; net_lock=0; }
typedef struct aster_app_ctx { uint64_t network_owner; uint32_t permissions; } aster_app_ctx_t;
typedef struct { aster_app_ctx_t ctx; uint8_t memory[8192]; } guest;
typedef guest *wasm_exec_env_t;
typedef guest *wasm_module_inst_t;
static aster_app_ctx_t *ctx_of(guest *g) {return &g->ctx;}
static bool aster_ctx_perm(aster_app_ctx_t *ctx,uint32_t bit) {return !!(ctx->permissions&bit);}
static guest *wasm_runtime_get_module_inst(guest *g) {return g;}
static bool wasm_runtime_get_app_addr_range(guest *g,uint32_t off,void *start,uint64_t *end)
{(void)g;(void)start;*end=8192;return off<8192;}
static void *wasm_runtime_addr_app_to_native(guest *g,uint32_t off) {return g->memory+off;}
#include "../systems/zephyr/runtime/aster_network_bridge.inc"
static void tick(void) {host_now++;service_tick();}
int main(void)
{
 _Static_assert(sizeof(aster_network_status_t)==56,"status layout");
 _Static_assert(sizeof(aster_network_packet_t)==32,"packet layout");
 guest a={0},b={0};guest *env=&a;
 assert(n_network_open(env,2,0,0,0,0)==ASTER_EPERM);
 assert(n_network_get_info(env,100,44)==ASTER_EPERM);
 a.ctx.permissions=ASTER_PERM_WIFI;
 assert(n_network_open(env,2,0,0,0,0)==ASTER_EPERM);
 a.ctx.permissions=b.ctx.permissions=ASTER_PERM_NET;
 assert(n_network_open(env,1,100,UINT32_MAX,80,10)==ASTER_EINVAL);
 assert(n_network_open(env,2,0,0,1,0)==ASTER_EINVAL);
 assert(n_network_get_info(env,100,43)==ASTER_EINVAL);
 assert(n_network_get_info(env,UINT32_MAX,44)==ASTER_EINVAL);
 assert(!n_network_get_info(env,100,44));
 aster_network_info_t info;memcpy(&info,a.memory+100,sizeof(info));
 assert(sizeof(info)==44&&info.flags==15&&info.max_connections==2&&info.udp_payload_max==1200);
 assert(!strcmp(info.ipv4,"127.0.0.1")&&!worker_stack);
 host_iface.lock=1;assert(n_network_get_info(env,100,44)==ASTER_EAGAIN);host_iface.lock=0;
 host_if_missing=1;assert(!n_network_get_info(env,100,44));memcpy(&info,a.memory+100,44);assert(!info.flags&&!info.ipv4[0]);host_if_missing=0;
 host_if_no_address=1;assert(!n_network_get_info(env,100,44));memcpy(&info,a.memory+100,44);assert(info.flags==7&&!info.ipv4[0]);host_if_no_address=0;
 int h=n_network_open(&a,2,0,0,0,0);assert(h>0);tick();
 int other=n_network_open(&b,2,0,0,0,0);assert(other>0);tick();
 assert(a.ctx.network_owner!=b.ctx.network_owner&&a.ctx.network_owner!=ASTER_NET_CLI_OWNER);
 assert(n_network_open(&a,2,0,0,0,0)==ASTER_ENOSPC);
 assert(n_network_close(&a,other)==ASTER_EBADF);
 assert(aster_net_close(ASTER_NET_CLI_OWNER,h)==-EBADF);
 assert(n_network_status(&a,h,UINT32_MAX,56)==ASTER_EINVAL);
 assert(n_network_status(&a,h,0,56)==ASTER_EINVAL);
 assert(n_network_status(&a,h,100,55)==ASTER_EINVAL);
 assert(!n_network_status(&b,other,100,56));
 aster_network_status_t s;memcpy(&s,b.memory+100,56);uint16_t port=s.local_port;
 strcpy((char *)a.memory+100,"127.0.0.1");memcpy(a.memory+200,"hello",5);
 assert(n_network_send(&a,h,UINT32_MAX,5,100,9,port)==ASTER_EINVAL);
 assert(n_network_send(&a,h,200,UINT32_MAX,100,9,port)==ASTER_EMSGSIZE);
 assert(n_network_send(&a,h,200,1201,100,9,port)==ASTER_EMSGSIZE);
 assert(n_network_send(&a,h,200,5,100,UINT32_MAX,port)==ASTER_EINVAL);
 assert(n_network_send(&a,h,200,5,100,9,65536)==ASTER_EINVAL);
 assert(n_network_send(&a,h,200,5,100,9,port)==5);
 memset(a.memory+200,0,5);tick();tick();tick();
 assert(n_network_recv(&b,other,500,5,8190,32)==ASTER_EINVAL);
 assert(n_network_recv(&b,other,500,5,100,31)==ASTER_EINVAL);
 assert(n_network_recv(&b,other,100,5,100,32)==ASTER_EINVAL);
 assert(n_network_recv(&b,other,UINT32_MAX,5,100,32)==ASTER_EINVAL);
 assert(n_network_recv(&b,other,500,UINT32_MAX,100,32)==ASTER_EINVAL);
 assert(n_network_recv(&b,other,500,4,100,32)==ASTER_EMSGSIZE);
 aster_network_packet_t p;memcpy(&p,b.memory+100,32);assert(p.length==5);
 assert(n_network_recv(&b,other,500,5,100,32)==5);
 memcpy(&p,b.memory+100,32);assert(p.length==5&&p.port&&strcmp(p.ipv4,"127.0.0.1")==0);
 assert(!memcmp(b.memory+500,"hello",5));
 assert(n_network_recv(&b,other,500,5,100,32)==ASTER_EAGAIN);
 assert(!n_network_send(&a,h,0,0,100,9,port));tick();tick();tick();
 assert(!n_network_recv(&b,other,0,0,100,32));
 assert(!n_network_cancel(&a,h));tick();
 assert(!n_network_status(&a,h,500,56));memcpy(&s,a.memory+500,56);
 assert(s.state==ASTER_NETWORK_CANCELLED&&s.error==ASTER_ECANCELLED);
 assert(n_network_send(&a,h,0,0,100,9,port)==ASTER_ECANCELLED);
 uint64_t retired=a.ctx.network_owner;
 net_lock=1;aster_native_release_network(&a.ctx);assert(slept==1&&!a.ctx.network_owner);tick();
 assert(n_network_close(&a,h)==ASTER_EBADF&&a.ctx.network_owner!=retired);
 aster_native_release_network(&a.ctx);aster_native_release_network(&b.ctx);tick();assert(!psram_live);
 /* DNS status and returned IO errors share the stable category. */
 strcpy((char *)a.memory+100,"missing.test");dns_mode=2;
 h=n_network_open(&a,1,100,12,80,100);assert(h>0);tick();tick();
 assert(!n_network_status(&a,h,500,56));memcpy(&s,a.memory+500,56);assert(s.error==ASTER_EDNS);
 assert(n_network_recv(&a,h,200,10,500,32)==ASTER_EDNS);
 assert(n_network_send(&a,h,200,1,0,0,0)==ASTER_EDNS);
 aster_native_release_network(&a.ctx);tick();assert(!psram_live);
 assert(network_error(-EPIPE)==ASTER_EPIPE&&network_error(-ETIMEDOUT)==ASTER_ETIMEDOUT);
 assert(network_error(-ECONNREFUSED)==ASTER_ECONNREFUSED&&network_error(-ECONNRESET)==ASTER_ECONNRESET);
 free(worker_stack);return 0;
}
