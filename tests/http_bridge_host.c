/* SPDX-License-Identifier: MIT: actual service + actual native bridge. */
#define ASTER_NET_HOST_TEST
#include "../systems/zephyr/services/aster_net.c"
#define ASTER_HTTP_HOST_TEST
#include "../systems/zephyr/services/aster_http.c"
#include <aster/aster.h>
#include "../systems/zephyr/services/aster_perms.h"
#define CONFIG_ASTER_NET 1
#define CONFIG_ASTER_HTTP 1
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

#include "../systems/zephyr/runtime/aster_http_bridge.inc"
static void tick(void) { ++host_now; service_tick(); http_tick(); }
int main(void)
{
    /* The sibling TCP/UDP imports have their own native bridge regression. */
    (void)n_network_get_info; (void)n_network_open; (void)n_network_send;
    (void)n_network_recv; (void)n_network_status; (void)n_network_cancel;
    (void)n_network_close;
    guest a = {0}, b = {0};
    _Static_assert(sizeof(aster_http_status_t) == 56, "ABI size");
    assert(n_http_request(&a, 1, 100, 1, 0, 0, 0, 0, 1000, 100) == ASTER_EPERM);
    a.ctx.permissions = ASTER_PERM_WIFI;
    assert(n_http_status(&a, 1, 100, 56) == ASTER_EPERM);
    a.ctx.permissions = b.ctx.permissions = ASTER_PERM_NET;
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(listener, (void *)&addr, sizeof(addr)) && !listen(listener, 4));
    socklen_t len = sizeof(addr);
    assert(!getsockname(listener, (void *)&addr, &len));
    assert(!fcntl(listener, F_SETFL, O_NONBLOCK));
    snprintf((char *)a.memory + 100, 200, "http://127.0.0.1:%u/test", ntohs(addr.sin_port));
    uint32_t un = strlen((char *)a.memory + 100);
    assert(n_http_request(&a, 1, UINT32_MAX, un, 0, 0, 0, 0, 1000, 100) == ASTER_EINVAL);
    assert(n_http_request(&a, 1, 100, UINT32_MAX, 0, 0, 0, 0, 1000, 100) == ASTER_EINVAL);
    assert(n_http_request(&a, 1, 100, un+1, 0, 0, 0, 0, 1000, 100) == ASTER_EINVAL);
    assert(n_http_request(&a, 1, 100, un, 300, 0, 0, 0, 1000, 100) == ASTER_EINVAL);
    assert(n_http_request(&a, 1, 100, un, 0, 0, 0, 1, 1000, 100) == ASTER_EINVAL);
    assert(n_http_request(&a, 1, 100, un, 0, 0, 0, 0, 0, 100) == ASTER_EINVAL);
    int h = n_http_request(&a, 1, 100, un, 0, 0, 0, 0, 10000, 100);
    assert(h > 0);
    /* Request URL was copied: overwrite guest memory before worker starts. */
    memset(a.memory+100, 'x', un);
    assert(n_http_status(&b, h, 500, 56) == ASTER_EBADF);
    assert(n_http_close(&b, h) == ASTER_EBADF);
    assert(n_http_cancel(&b, h) == ASTER_EBADF);
    assert(n_http_status(&a, h, 500, 55) == ASTER_EINVAL);
    assert(n_http_status(&a, h, UINT32_MAX, 56) == ASTER_EINVAL);
    assert(n_http_read(&a, h, 500, 0) == ASTER_EINVAL);
    assert(n_http_read(&a, h, 8190, 8) == ASTER_EINVAL);
    int remote = -1;
    char request[2048] = {0};
    for (unsigned i=0; i<10000 && !strstr(request,"\r\n\r\n"); ++i) {
        tick();
        if (remote < 0) { remote=accept(listener,NULL,NULL); if(remote>=0) fcntl(remote,F_SETFL,O_NONBLOCK); }
        if(remote>=0) {
            size_t n=strlen(request); int rc=recv(remote,request+n,sizeof(request)-n-1,0);
            if(rc>0) request[n+rc]=0;
        }
    }
    assert(strstr(request,"GET /test HTTP/1.1"));
    const char *wire="HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-Test: one\r\nX-Test: two\r\n\r\nhello";
    assert(send(remote,wire,strlen(wire),0)==(int)strlen(wire));
    aster_http_status_t status={0};
    for(unsigned i=0;i<10000;++i) {
        tick(); assert(!n_http_status(&a,h,501,60)); memcpy(&status,a.memory+501,56);
        if(status.state==ASTER_HTTP_STATE_DONE)break;
    }
    assert(status.state==ASTER_HTTP_STATE_DONE && status.status==200 && !status.error);
    assert(status.received==5 && status.content_length==5 && status.queued==5 && status.flags==3);
    assert(!status.certificate_flags && !status.reserved);
    assert(n_http_read(&a,h,8190,5)==ASTER_EINVAL); /* invalid read must not consume */
    strcpy((char *)a.memory+100,"x-test");
    assert(n_http_header(&a,h,100,6,1,600,2,700)==ASTER_EMSGSIZE);
    uint32_t required;memcpy(&required,a.memory+700,4);assert(required==3);
    assert(n_http_header(&a,h,100,6,1,600,4,601)==ASTER_EINVAL);
    assert(n_http_header(&a,h,100,6,1,600,4,8190)==ASTER_EINVAL);
    assert(n_http_header(&a,h,100,6,1,600,4,701)==3);
    assert(!memcmp(a.memory+600,"two",3));
    assert(n_http_header(&a,h,100,6,2,600,4,701)==ASTER_ENOENT);
    assert(n_http_read(&a,h,601,5)==5 && !memcmp(a.memory+601,"hello",5));
    assert(!n_http_read(&a,h,601,5));
    struct aster_http_snapshot snap={.tls_verify=4,.tls_error=-1};
    assert(http_error(-EACCES,&snap)==ASTER_ECERT);
    assert(http_error(-EPROTO,&snap)==ASTER_EPROTO);
    assert(http_error(-ELOOP,&snap)==ASTER_EREDIRECT);
    assert(http_error(-ENODATA,&snap)==ASTER_ECLOCK);
    assert(http_error(-EIO,&snap)==ASTER_ETLS);
    snap.tls_verify=UINT32_MAX; assert(http_error(-EACCES,&snap)==ASTER_EPERM);
    snap.transport_error=-EPIPE; assert(http_error(-EIO,&snap)==ASTER_EIO);
    uint64_t old_owner=a.ctx.network_owner;
    aster_native_release_network(&a.ctx);
    assert(!a.ctx.network_owner && !slept);
    assert(n_http_status(&a,h,500,56)==ASTER_EBADF && a.ctx.network_owner!=old_owner);
    tick();tick();
    struct aster_http_stats stats;assert(!aster_http_svc_get_stats(&stats) && !stats.active);
    assert(n_http_read(&a,h,500,5)==ASTER_EBADF);
    close(remote);close(listener);
    puts("HTTP bridge bounds, ownership, copied inputs and cleanup OK");
    return 0;
}
