/* SPDX-License-Identifier: MIT */
#define ASTER_NET_HOST_TEST
#define ASTER_HTTP_HOST_TEST
#define ASTER_WS_HOST_TEST
#define CONFIG_ASTER_WEBSOCKET 1
#include "../systems/zephyr/services/aster_net.c"
#ifdef ASTER_WS_TLS_TEST
#define CONFIG_ASTER_HTTP_TLS 1
#include "../systems/zephyr/services/aster_http_tls.h"
struct aster_http_tls { uint64_t owner; uint32_t handle; };
static int fake_tls_error;
const char *aster_http_tls_default_ca(size_t *length)
{ static const char ca[]="test-only CA"; *length=sizeof(ca);return ca; }
int aster_http_tls_new(struct aster_http_tls **out,uint64_t owner,uint32_t handle,
 const char *host,const char *pem,size_t length,struct aster_http_tls_report *report)
{ (void)host;(void)report;assert(pem&&length);*out=aster_psram_alloc(sizeof(**out));
 if(!*out)return -ENOMEM;
 (*out)->owner=owner;(*out)->handle=handle;return 0; }
int aster_http_tls_handshake(struct aster_http_tls *t,struct aster_http_tls_report *report)
{(void)t;if(fake_tls_error)report->verify=1;return fake_tls_error;}
int aster_http_tls_send(struct aster_http_tls *t,const void *p,size_t n,struct aster_http_tls_report *report)
{(void)report;return aster_net_send(t->owner,t->handle,p,n,NULL,0);}
int aster_http_tls_recv(struct aster_http_tls *t,void *p,size_t n,struct aster_http_tls_report *report)
{(void)report;size_t required;return aster_net_recv(t->owner,t->handle,p,n,&required,NULL,NULL);}
int aster_http_tls_free(struct aster_http_tls **p){aster_psram_free(*p);*p=NULL;return 0;}
size_t aster_http_tls_context_bytes(void){return sizeof(struct aster_http_tls);}
void aster_http_tls_heap(size_t *current,size_t *peak){*current=*peak=0;}
#endif
static int rng_fail;
static int sys_csrand_get(void *data,size_t n)
{static unsigned seq;uint8_t *p=data;if(rng_fail)return -EIO;while(n--)*p++=++seq;return 0;}
#include "../systems/zephyr/services/aster_http.c"
#include "../systems/zephyr/services/aster_ws.c"
#include "../systems/zephyr/services/aster_ws_codec.c"
#include <signal.h>
#include <netinet/tcp.h>
#include <stdio.h>
static int server,ws_peer=-1;static uint16_t port;
static void pump(void)
{
    host_now++;service_tick();http_tick();aster_ws_svc_tick();
    if(ws_peer<0){ws_peer=accept(server,NULL,NULL);if(ws_peer>=0){int one=1;setsockopt(ws_peer,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));fcntl(ws_peer,F_SETFL,O_NONBLOCK);}}
}
static void steps(unsigned n){while(n--)pump();}
static int open_client(void)
{char url[96];snprintf(url,sizeof(url),"%s://127.0.0.1:%u/echo",
#ifdef ASTER_WS_TLS_TEST
 "wss",
#else
 "ws",
#endif
 port);int h=aster_ws_svc_open(2,url,10000);assert(h>0);return h;}
static void upgrade(void)
{
    char b[2048]={0};size_t n=0;
    for(unsigned i=0;i<10000;i++){pump();if(ws_peer>=0){int r=recv(ws_peer,b+n,sizeof(b)-n-1,MSG_DONTWAIT);if(r>0)n+=r;}if(strstr(b,"\r\n\r\n"))break;}
    char *key=strstr(b,"Sec-WebSocket-Key: ");assert(key);key+=19;assert(key[24]=='\r');key[24]=0;
    uint8_t digest[20];char expected[29];accept_digest(key,digest);base64(digest,20,expected);
    int r=snprintf(b,sizeof(b),"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",expected);
    assert(send(ws_peer,b,r,0)==r);steps(400);
    assert(ws_client&&ws_client->pub.state==ASTER_WS_READY);
}
static void wire(const uint8_t *p,size_t n){assert(send(ws_peer,p,n,0)==(ssize_t)n);steps(100);}
static void expect_client(unsigned expected,const uint8_t *payload,size_t length)
{
    uint8_t b[ASTER_WS_WIRE_MAX];size_t used=0,header=0,total=0;
    for(unsigned k=0;k<10000;k++){
        pump();struct pollfd event={.fd=ws_peer,.events=POLLIN};poll(&event,1,1);int n=recv(ws_peer,b+used,sizeof(b)-used,MSG_DONTWAIT);if(n>0)used+=n;
        if(used<2)continue;
        size_t amount=b[1]&127;header=6;
        if(amount==126){if(used<4)continue;amount=((unsigned)b[2]<<8)|b[3];header=8;}
        assert(amount!=127&&(b[0]&128)&&(b[1]&128));total=header+amount;
        if(used==total)break;
    }
    if(!(used==total&&total==header+length&&(b[0]&15)==expected))fprintf(stderr,"client expected=%u n=%zu used=%zu total=%zu header=%zu opcode=%u state=%u error=%d now=%lld\n",expected,length,used,total,header,b[0]&15,ws_client->pub.state,ws_client->pub.error,(long long)host_now);
    assert(used==total&&total==header+length&&(b[0]&15)==expected);
    for(size_t i=0;i<length;i++)assert((b[header+i]^b[header-4+i%4])==payload[i]);
}
static void drop(unsigned h)
{assert(!aster_ws_svc_close(2,h));steps(1700);assert(!ws_client);if(ws_peer>=0){close(ws_peer);ws_peer=-1;}assert(!psram_live);}
int main(void)
{
    signal(SIGPIPE,SIG_IGN);server=socket(AF_INET,SOCK_STREAM,0);assert(server>=0);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(server,(struct sockaddr *)&a,sizeof(a)));assert(!listen(server,4));fcntl(server,F_SETFL,O_NONBLOCK);
    socklen_t len=sizeof(a);assert(!getsockname(server,(struct sockaddr *)&a,&len));port=ntohs(a.sin_port);
    unsigned h=open_client();upgrade();assert(aster_ws_svc_open(3,"ws://127.0.0.1/",1000)==-ENOSPC);
#ifdef ASTER_WS_TLS_TEST
    struct aster_http_options http={.method=ASTER_HTTP_GET,.url="https://127.0.0.1/",.timeout_ms=1000,.body_limit=32};
    assert(aster_http_svc_start(3,&http)==-EBUSY);
    assert(aster_http_svc_ca_set(NULL,0)==-EBUSY);
#endif
    struct aster_ws_snapshot s;assert(aster_ws_svc_status(3,h,&s)==-EBADF);
    const uint8_t first[]={1,2,0xe4,0xb8},ping[]={0x89,1,0x77},last[]={0x80,1,0xad};
    wire(first,sizeof(first));wire(ping,sizeof(ping));wire(last,sizeof(last));
    uint8_t data[4096];size_t n;unsigned op;
    expect_client(10,ping+2,1);
    assert(aster_ws_svc_recv(2,h,data,2,&n,&op)==-EMSGSIZE&&n==3&&op==1);
    assert(!aster_ws_svc_recv(2,h,data,sizeof(data),&n,&op)&&n==3&&!memcmp(data,"\xe4\xb8\xad",3));
    /* Empty message remains distinguishable from no message/closed. */
    const uint8_t empty[]={0x82,0};wire(empty,sizeof(empty));assert(!aster_ws_svc_recv(2,h,NULL,0,&n,&op)&&n==0&&op==2);
    memset(data,0x37,sizeof(data));assert(!aster_ws_svc_send(2,h,2,data,sizeof(data)));assert(aster_ws_svc_send(2,h,2,data,1)==-EAGAIN);steps(100);
    expect_client(2,data,sizeof(data));
    const uint8_t queued_a[]={0x82,1,0x41},queued_b[]={0x82,1,0x42};
    wire(queued_a,sizeof(queued_a));wire(queued_b,sizeof(queued_b));
    assert(!aster_ws_svc_recv(2,h,data,sizeof(data),&n,&op)&&n==1&&data[0]==0x41);steps(100);
    assert(!aster_ws_svc_recv(2,h,data,sizeof(data),&n,&op)&&n==1&&data[0]==0x42);
    assert(aster_ws_svc_send(3,h,2,data,1)==-EBADF);drop(h);
    for(unsigned i=0;i<100;i++){h=open_client();assert(!aster_ws_svc_cancel(2,h));steps(5);drop(h);}
    h=open_client();upgrade();const uint8_t bad[]={0x81,0x80,0,0,0,0};wire(bad,sizeof(bad));assert(ws_client->pub.state==ASTER_WS_FAILED);drop(h);
    h=open_client();upgrade();const uint8_t huge[]={0x82,126,0x10,1};wire(huge,sizeof(huge));assert(ws_client->pub.error==-EMSGSIZE);drop(h);
    h=open_client();upgrade();const uint8_t bye[]={0x88,2,3,232};wire(bye,sizeof(bye));steps(50);assert(ws_client->pub.state==ASTER_WS_CLOSED&&ws_client->pub.close_code==1000);drop(h);
    rng_fail=1;h=open_client();steps(1000);assert(ws_client->pub.error==-EIO);rng_fail=0;drop(h);
#ifdef ASTER_WS_TLS_TEST
    fake_tls_error=-EACCES;h=open_client();steps(1000);assert(ws_client->pub.error==-EACCES&&ws_client->pub.tls_verify==1);fake_tls_error=0;drop(h);
#endif
    close(server);puts("WS fragmentation, control, limits and 100 lifecycle recovery OK");return 0;
}
