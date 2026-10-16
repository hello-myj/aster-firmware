/* SPDX-License-Identifier: MIT. Actual native bridge, worker and TCP wire. */
#define main mqtt_service_regression_main
#include "mqtt_service_host.c"
#undef main
#include <aster/aster.h>
#include "../systems/zephyr/services/aster_perms.h"
#define CONFIG_ASTER_NET 1
#define CONFIG_ASTER_HTTP 1
#define IS_ENABLED(n) (n)
#define zsock_inet_ntop inet_ntop
static void k_sleep(int n) { host_now+=n; }
typedef struct aster_app_ctx { uint64_t network_owner; uint32_t permissions; } aster_app_ctx_t;
typedef struct { aster_app_ctx_t ctx; uint8_t memory[8192]; } guest;
typedef guest *wasm_exec_env_t;
typedef guest *wasm_module_inst_t;
static aster_app_ctx_t *ctx_of(guest *g) {return &g->ctx;}
static bool aster_ctx_perm(aster_app_ctx_t *c,uint32_t b) {return !!(c->permissions&b);}
static guest *wasm_runtime_get_module_inst(guest *g) {return g;}
static bool wasm_runtime_get_app_addr_range(guest *g,uint32_t o,void *s,uint64_t *e)
{(void)g;(void)s;*e=8192;return o<8192;}
static void *wasm_runtime_addr_app_to_native(guest *g,uint32_t o) {return g->memory+o;}
#include "../systems/zephyr/runtime/aster_network_bridge.inc"
#include "../systems/zephyr/runtime/aster_http_bridge.inc"
#include "../systems/zephyr/runtime/aster_mqtt_bridge.inc"
int main(void)
{
    (void)n_network_get_info;(void)n_network_open;(void)n_network_send;
    (void)n_network_recv;(void)n_network_status;(void)n_network_cancel;(void)n_network_close;
    (void)n_http_request;(void)n_http_status;(void)n_http_read;(void)n_http_header;
    (void)n_http_cancel;(void)n_http_close;
    signal(SIGPIPE,SIG_IGN);dns_mode=1;
    struct aster_mqtt_snapshot rejected={.tls_verify=4};
    assert(mqtt_error(-EACCES,&rejected)==ASTER_ECERT);
    assert(mqtt_error(-EPROTO,NULL)==ASTER_EPROTO);
    guest a={0},b={0};
    assert(n_mqtt_connect(&a,100,80)==ASTER_EPERM);
    a.ctx.permissions=ASTER_PERM_WIFI;
    assert(n_mqtt_recv(&a,1,100,1176)==ASTER_EPERM);
    a.ctx.permissions=b.ctx.permissions=ASTER_PERM_NET;
    broker=socket(AF_INET,SOCK_STREAM,0);assert(broker>=0);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(broker,(void*)&addr,sizeof(addr))&&!listen(broker,4));
    socklen_t n=sizeof(addr);assert(!getsockname(broker,(void*)&addr,&n));
    fcntl(broker,F_SETFL,O_NONBLOCK);broker_port=ntohs(addr.sin_port);
    memcpy(a.memory+200,"127.0.0.1",9);memcpy(a.memory+300,"bridge-test",11);
    aster_mqtt_options_t options={.size=80,.version=1,.port=broker_port,
        .timeout_ms=10000,.keepalive_s=5,.host={200,9},.client_id={300,11}};
    memcpy(a.memory+101,&options,80);
    assert(n_mqtt_connect(&a,101,79)==ASTER_EINVAL);
    assert(n_mqtt_connect(&a,8190,80)==ASTER_EINVAL);
    options.version=2;memcpy(a.memory+101,&options,80);
    assert(n_mqtt_connect(&a,101,80)==ASTER_EINVAL);
    options.version=1;options.password=(aster_string_t){8190,20};memcpy(a.memory+101,&options,80);
    assert(n_mqtt_connect(&a,101,80)==ASTER_EINVAL);
    options.password=(aster_string_t){0,0};memcpy(a.memory+101,&options,80);
    int h=n_mqtt_connect(&a,101,80);assert(h>0);
    /* Host/client/options lifetime independent of guest after connect. */
    memset(a.memory+101,0,300);connected(h);
    assert(n_mqtt_status(&b,h,501,64)==ASTER_EBADF);
    assert(n_mqtt_cancel(&b,h)==ASTER_EBADF);
    assert(n_mqtt_close(&b,h)==ASTER_EBADF);
    assert(n_mqtt_status(&a,h,8190,64)==ASTER_EINVAL);
    assert(!n_mqtt_status(&a,h,501,68));
    aster_mqtt_status_t status;memcpy(&status,a.memory+501,64);
    assert(status.size==64&&status.version==1&&status.state==4&&!status.reserved);
    memcpy(a.memory+200,"a",1);memcpy(a.memory+300,"binary\0ok",9);
    assert(n_mqtt_publish(&a,h,200,1,8190,10,0,0)==ASTER_EINVAL);
    assert(n_mqtt_publish(&a,h,200,1,300,1025,0,0)==ASTER_EMSGSIZE);
    assert(n_mqtt_publish(&a,h,200,1,0,0,2,0)==ASTER_EINVAL);
    int op=n_mqtt_publish(&a,h,200,1,300,9,1,0);assert(op>0);
    memset(a.memory+300,'x',9);
    assert(n_mqtt_operation(&a,h,op,8190)==ASTER_EINVAL);
    assert(n_mqtt_operation(&a,h,op,701)==ASTER_EAGAIN);
    unsigned id=broker_packet(3);uint8_t ack[]={0x40,2,id>>8,id&255};wire(ack,4);
    assert(!n_mqtt_operation(&a,h,op,701));int32_t error;memcpy(&error,a.memory+701,4);assert(!error);
    assert(n_mqtt_operation(&a,h,op+100,701)==ASTER_ENOENT);
    assert(n_mqtt_subscribe(&a,h,200,1,2)==ASTER_EINVAL);
    op=n_mqtt_subscribe(&a,h,200,1,1);assert(op>0);id=broker_packet(8);
    uint8_t suback[]={0x90,3,id>>8,id&255,1};wire(suback,5);
    op=n_mqtt_unsubscribe(&a,h,200,1);assert(op>0);id=broker_packet(10);
    uint8_t unsuback[]={0xb0,2,id>>8,id&255};wire(unsuback,4);
    /* Invalid destination must not consume a queued empty/binary message. */
    wire("\x30\x06\x00\x01" "a\0xy",8);
    assert(n_mqtt_recv(&a,h,8190,1176)==ASTER_EINVAL);
    assert(n_mqtt_recv(&a,h,1001,1175)==ASTER_EINVAL);
    assert(!n_mqtt_recv(&a,h,1001,1180));
    aster_mqtt_message_t m;memcpy(&m,a.memory+1001,sizeof(m));
    assert(m.size==1176&&m.version==1&&m.topic_length==1&&m.topic[0]=='a');
    assert(m.length==3&&!memcmp(m.data,"\0xy",3)&&!m.flags&&m.data[3]==0);
    assert(n_mqtt_recv(&a,h,1001,1176)==ASTER_EAGAIN);
    assert(!n_mqtt_cancel(&a,h));pump();
    assert(!n_mqtt_close(&a,h));
    assert(n_mqtt_status(&a,h,501,64)==ASTER_EBADF);
    for(unsigned i=0;i<1600;++i) pump();
    close(mqtt_peer);mqtt_peer=-1;broker_length=0;
    /* The runtime uses this same cleanup for stop/trap/init failure. */
    memcpy(a.memory+200,"127.0.0.1",9);memcpy(a.memory+300,"bridge-test",11);
    memcpy(a.memory+101,&options,80);h=n_mqtt_connect(&a,101,80);assert(h>0);connected(h);
    uint64_t retired=a.ctx.network_owner;aster_native_release_network(&a.ctx);
    assert(!a.ctx.network_owner);broker_packet(14);
    for(unsigned i=0;i<1600;++i) pump();
    close(mqtt_peer);mqtt_peer=-1;
    assert(!psram_live);
    assert(n_mqtt_status(&a,h,501,64)==ASTER_EBADF&&a.ctx.network_owner!=retired);
    close(broker);free(worker_stack);free(http_stack);
    puts("MQTT bridge guest boundaries, copy, ownership and exit cleanup OK");
}
