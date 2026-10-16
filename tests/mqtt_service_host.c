/* SPDX-License-Identifier: MIT
 * Actual MQTT/HTTP/TCP workers over POSIX loopback. Deterministic broker wire.
 */
#define ASTER_NET_HOST_TEST
#define ASTER_HTTP_HOST_TEST
#define ASTER_MQTT_HOST_TEST
#define CONFIG_ASTER_MQTT 1
#include "../systems/zephyr/services/aster_net.c"
#ifdef ASTER_MQTT_TLS_TEST
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
#include "../systems/zephyr/services/aster_http.c"
#include "../systems/zephyr/services/aster_mqtt.c"
#include <signal.h>
#include <netinet/tcp.h>
#include <stdio.h>
static int broker, mqtt_peer = -1;
static uint8_t broker_input[8192];
static size_t broker_length;
static uint16_t broker_port;
static bool use_test_tls;
static void pump(void)
{
    ++host_now; service_tick(); http_tick(); aster_mqtt_svc_tick();
    if (mqtt_peer < 0 && broker >= 0) {
        mqtt_peer = accept(broker, NULL, NULL);
        if (mqtt_peer >= 0) { int one = 1; setsockopt(mqtt_peer, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); fcntl(mqtt_peer, F_SETFL, O_NONBLOCK); broker_length = 0; }
    }
    if (mqtt_peer >= 0) {
        int n = recv(mqtt_peer, broker_input + broker_length,
                     sizeof(broker_input) - broker_length, MSG_DONTWAIT);
        if (n > 0) broker_length += n;
    }
}
static unsigned broker_packet(unsigned type)
{
    for (unsigned i = 0; i < 10000; ++i) {
        pump();
        size_t n = 0, h = 1, mult = 1;
        while (h < broker_length && h < 5) {
            uint8_t b = broker_input[h++]; n += (b & 127) * mult; mult *= 128;
            if (!(b & 128)) {
                if (broker_length < h + n) break;
                assert(broker_input[0] >> 4 == type);
                unsigned id = n >= 2 ? ((unsigned)broker_input[h] << 8) | broker_input[h + 1] : 0;
                if (type == 3) {
                    unsigned topic = id;
                    assert(topic < n); id = ((unsigned)broker_input[h + 2 + topic] << 8) | broker_input[h + 3 + topic];
                }
                if (type == 1) assert(!memcmp(broker_input + h, "\x00\x04MQTT\x04\x02", 8));
                broker_length -= h + n; memmove(broker_input, broker_input + h + n, broker_length);
                return id;
            }
        }
    }
    fprintf(stderr, "broker wanted type=%u buffered=%zu state=%u now=%lld\n",type,broker_length,
       mqtt_clients[0] ? mqtt_clients[0]->pub.state : 0,(long long)host_now);
    abort();
}
static void wire(const void *p, size_t n)
{
    assert(mqtt_peer >= 0); assert(send(mqtt_peer, p, n, 0) == (ssize_t)n);
    for (unsigned i = 0; i < 12; ++i) pump();
}
static struct aster_mqtt_snapshot snapshot(unsigned h)
{
    struct aster_mqtt_snapshot s; assert(!aster_mqtt_svc_status(2, h, &s)); return s;
}
static int open_client(bool reconnect)
{
    struct aster_mqtt_connect o = {.host="127.0.0.1",.client_id="aster-host",
        .port=broker_port,.timeout_ms=10000,.keepalive_s=5,.reconnect=reconnect,.tls=use_test_tls};
    return aster_mqtt_svc_open(2, &o);
}
static void connected(int h)
{
    broker_packet(1);
    wire("\x20",1); assert(snapshot(h).state==ASTER_MQTT_CONNACK);
    wire("\x02\x00\x00",3); assert(snapshot(h).state==ASTER_MQTT_READY);
}
static void dispose(int h, bool graceful)
{
    assert(!aster_mqtt_svc_close(2,h));
    assert(aster_mqtt_svc_status(2,h,&(struct aster_mqtt_snapshot){0})==-EBADF);
    if(graceful) broker_packet(14);
    for(unsigned i=0;i<1600;++i)pump();
    if(mqtt_peer>=0) close(mqtt_peer);
    mqtt_peer=-1;broker_length=0;
    for(unsigned i=0;i<5;++i)pump();
    assert(!psram_live);
}
static void framing(void)
{
    struct aster_mqtt_packet p;
    assert(aster_mqtt_topic("sensors/+/state",true)==0);
    assert(aster_mqtt_topic("sensors/#/state",true)==-EINVAL);
    assert(aster_mqtt_topic("sensors/ab+",true)==-EINVAL);
    assert(aster_mqtt_topic("sensors/+",false)==-EINVAL);
    assert(aster_mqtt_topic("\xc0\x80",false)==-EINVAL);
    assert(aster_mqtt_topic("\xed\xa0\x80",false)==-EINVAL);
    assert(aster_mqtt_topic("\xf4\x90\x80\x80",false)==-EINVAL);
    assert(aster_mqtt_topic("\xe4\xb8\xad/\xe6\x96\x87",false)==0);
    assert(aster_mqtt_decode((uint8_t*)"\x20\x80\x00",3,&p)==-EPROTO);
    assert(aster_mqtt_decode((uint8_t*)"\x30\x80\x80\x80\x80",5,&p)==-EPROTO);
    assert(aster_mqtt_decode((uint8_t*)"\x30\xff\x7f",3,&p)==-EMSGSIZE);
    assert(aster_mqtt_decode((uint8_t*)"\x40\x02\x00\x00",4,&p)==-EPROTO);
    assert(aster_mqtt_decode((uint8_t*)"\x20\x02\x01\x00",4,&p)==-EPROTO);
    assert(aster_mqtt_decode((uint8_t*)"\x90\x03\x00\x01\x02",5,&p)==-EPROTO);
    uint8_t output[1536], payload[1024];memset(payload,0xa5,sizeof(payload));
    int n=aster_mqtt_encode_publish(output,sizeof(output),"a",payload,sizeof(payload),1,true,42);
    assert(n>1024);
    for(int i=0;i<n;++i)assert(!aster_mqtt_decode(output,i,&p));
    assert(aster_mqtt_decode(output,n,&p)==n&&p.id==42&&p.qos==1&&p.retain&&p.payload_length==1024);
    assert(!memcmp(p.payload,payload,sizeof(payload)));
    assert(aster_mqtt_encode_publish(output,8,"long",payload,100,0,false,0)==-EMSGSIZE);
    /* Deterministic malformed/random input under ASan/UBSan. */
    uint32_t seed=7;
    for(unsigned i=0;i<20000;++i){
        size_t size=i%sizeof(output);
        for(size_t j=0;j<size;++j){seed=seed*1664525+1013904223;output[j]=seed>>24;}
        (void)aster_mqtt_decode(output,size,&p);
    }
}
static void messaging(void)
{
    int raw = aster_net_open(2, ASTER_NET_UDP, NULL, 0, 5000);
    assert(raw > 0);
    int h=open_client(true);assert(h>0 && h != raw);connected(h);
    assert(aster_mqtt_svc_status(2,raw,&(struct aster_mqtt_snapshot){0})==-EBADF);
    assert(aster_net_snapshot(2,h,&(struct aster_net_snapshot){0})==-EBADF);
    assert(!aster_net_close(2,raw));pump();
    assert(aster_mqtt_svc_status(99,h,&(struct aster_mqtt_snapshot){0})==-EBADF);
    int op=aster_mqtt_svc_subscribe(2,h,"aster/+",1,false);assert(op>0);
    unsigned id=broker_packet(8);uint8_t ack[]={0x90,3,id>>8,id&255,1};wire(ack,sizeof(ack));
    assert(snapshot(h).last_operation==(unsigned)op&&snapshot(h).subscriptions==1);
    op=aster_mqtt_svc_publish(2,h,"aster/test","abc",3,1,true);assert(op>0);
    id=broker_packet(3);assert(snapshot(h).pending==1&&snapshot(h).sent==0);
    int32_t operation_error=99;
    assert(aster_mqtt_svc_operation(2,h,op,&operation_error)==-EAGAIN);
    uint8_t puback[]={0x40,2,id>>8,id&255};wire(puback,sizeof(puback));
    assert(snapshot(h).last_operation==(unsigned)op&&snapshot(h).sent==1);
    assert(!aster_mqtt_svc_operation(2,h,op,&operation_error)&&operation_error==0);
    assert(aster_mqtt_svc_operation(99,h,op,&operation_error)==-EBADF);
    assert(aster_mqtt_svc_publish(2,h,"aster/test",NULL,0,0,false)>0);
    broker_packet(3);assert(snapshot(h).sent==2);
    /* Duplicate QoS1, binary and zero payload preserve message boundaries. */
    uint8_t publish[1536];int n=aster_mqtt_encode_publish(publish,sizeof(publish),"aster/rx","x\0z",3,1,true,37);
    publish[0]|=8;wire(publish,n);assert(broker_packet(4)==37);
    struct aster_mqtt_message m;assert(!aster_mqtt_svc_recv(2,h,&m));
    assert(m.length==3&&m.qos==1&&m.dup&&m.retain&&!memcmp(m.data,"x\0z",3));
    wire(publish,n);assert(broker_packet(4)==37);assert(!aster_mqtt_svc_recv(2,h,&m));
    n=aster_mqtt_encode_publish(publish,sizeof(publish),"aster/rx",NULL,0,0,false,0);
    wire(publish,n);assert(!aster_mqtt_svc_recv(2,h,&m)&&m.length==0);
    assert(aster_mqtt_svc_recv(2,h,&m)==-EAGAIN);
    /* Queue pressure never overwrites an older application message. */
    wire(publish,n);wire(publish,n);wire(publish,n);assert(snapshot(h).queued==2);
    for(unsigned i=0;i<3;++i){assert(!aster_mqtt_svc_recv(2,h,&m));pump();}
    /* Keepalive must work without any APP tick/status calls. */
    host_now+=5000;broker_packet(12);wire("\xd0\x00",2);
    assert(snapshot(h).state==ASTER_MQTT_READY);
    /* Broker restart retains quota and restores acknowledged filters. */
    close(mqtt_peer);mqtt_peer=-1;
    for(unsigned i=0;i<50;++i)pump();
    assert(snapshot(h).state==ASTER_MQTT_BACKOFF);
    assert(aster_mqtt_svc_publish(2,h,"aster/test","x",1,0,false)==-ENOTCONN);
    host_now+=1001;connected(h);assert(snapshot(h).reconnects==1);
    id=broker_packet(8);ack[2]=id>>8;ack[3]=id&255;wire(ack,sizeof(ack));assert(snapshot(h).subscriptions==1);
    /* Rejected SUBACK does not create a confirmed subscription. */
    op=aster_mqtt_svc_subscribe(2,h,"forbidden",1,false);id=broker_packet(8);
    ack[2]=id>>8;ack[3]=id&255;ack[4]=0x80;wire(ack,sizeof(ack));
    assert(snapshot(h).last_operation==(unsigned)op&&snapshot(h).operation_error==-EACCES);
    op=aster_mqtt_svc_subscribe(2,h,"aster/+",0,true);id=broker_packet(10);
    uint8_t unack[]={0xb0,2,id>>8,id&255};wire(unack,sizeof(unack));
    assert(snapshot(h).subscriptions==0&&snapshot(h).last_operation==(unsigned)op);
    dispose(h,true);
}
static void failures(void)
{
    int h=open_client(true);assert(h>0);broker_packet(1);wire("\x20\x02\x00\x05",4);
    assert(snapshot(h).state==ASTER_MQTT_FAILED&&snapshot(h).connack==5&&snapshot(h).error==-EACCES);
    dispose(h,false);
    h=open_client(false);connected(h);
    assert(aster_mqtt_svc_publish(2,h,"a","x",1,1,false)>0);broker_packet(3);
    host_now+=10001;pump();assert(snapshot(h).state==ASTER_MQTT_FAILED&&snapshot(h).error==-ETIMEDOUT);
    dispose(h,false);
    h=open_client(false);connected(h);assert(!aster_mqtt_svc_cancel(2,h));pump();
    assert(snapshot(h).state==ASTER_MQTT_CANCELLED);dispose(h,false);
    h=open_client(false);connected(h);wire("\x30\xff\x7f",3);
    assert(snapshot(h).state==ASTER_MQTT_FAILED&&snapshot(h).error==-EMSGSIZE);dispose(h,false);
    h=open_client(false);connected(h);
    assert(aster_mqtt_svc_publish(2,h,"a","x",1,1,false)>0);
    assert(aster_mqtt_svc_publish(2,h,"a","y",1,1,false)>0);
    assert(aster_mqtt_svc_publish(2,h,"a","z",1,1,false)==-EAGAIN);
    assert(!aster_mqtt_svc_close_owner(99));assert(snapshot(h).pending==2);
    assert(!aster_mqtt_svc_close_owner(2));broker_packet(14);
    for(unsigned i=0;i<1600;++i) pump();
    close(mqtt_peer);mqtt_peer=-1;assert(!psram_live);
    /* Initially refused TCP connect must recover, not just an established FIN. */
    close(broker); broker=-1;
    h=open_client(true);assert(h>0);
    for(unsigned i=0;i<50;++i)pump();
    assert(snapshot(h).state==ASTER_MQTT_BACKOFF);
    broker=socket(AF_INET,SOCK_STREAM,0);assert(broker>=0);
    int one=1;setsockopt(broker,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=htons(broker_port)};
    assert(!bind(broker,(void*)&a,sizeof(a)));assert(!listen(broker,8));fcntl(broker,F_SETFL,O_NONBLOCK);
    host_now+=1001;connected(h);assert(snapshot(h).reconnects==1);dispose(h,true);
    fail_alloc=1;assert(open_client(false)==-ENOMEM);fail_alloc=0;
    for(unsigned i=0;i<100;++i){h=open_client(false);assert(h>0);connected(h);dispose(h,true);}
}
#ifdef ASTER_MQTT_TLS_TEST
/* Mock crypto only verifies admission/lease ownership. Physical MQTTS gates
 * must separately test the real TLS backend and certificate rejection. */
static void tls_admission(void)
{
    use_test_tls=true;
    int h=open_client(false);assert(h>0);connected(h);
    assert(aster_http_svc_ca_set(NULL,0)==-EBUSY);
    assert(open_client(false)==-EBUSY);
    struct aster_http_options http={.method=ASTER_HTTP_GET,.url="https://localhost:1/",.timeout_ms=1000,.body_limit=100};
    assert(aster_http_svc_start(3,&http)==-EBUSY);
    dispose(h,true);assert(!external_tls_reserved);
    int request=aster_http_svc_start(3,&http);assert(request>0);
    assert(open_client(false)==-EBUSY);
    assert(!aster_http_svc_close(3,request));
    for(unsigned i=0;i<50;++i)pump();
    assert(!psram_live);
    fake_tls_error=-EAGAIN;h=open_client(false);assert(h>0);
    for(unsigned i=0;i<50;++i)pump();
    assert(snapshot(h).state==ASTER_MQTT_HANDSHAKE);
    assert(!aster_mqtt_svc_cancel(2,h));pump();
    assert(snapshot(h).state==ASTER_MQTT_CANCELLED);dispose(h,false);
    assert(!external_tls_reserved);
    fake_tls_error=-EACCES;h=open_client(true);assert(h>0);
    for(unsigned i=0;i<50;++i)pump();
    assert(snapshot(h).state==ASTER_MQTT_FAILED&&snapshot(h).error==-EACCES);
    dispose(h,false);fake_tls_error=0;
    assert(!external_tls_reserved&& !psram_live);
    use_test_tls=false;
}
#endif
int main(void)
{
    signal(SIGPIPE,SIG_IGN);framing();dns_mode=1;
    broker=socket(AF_INET,SOCK_STREAM,0);assert(broker>=0);
    int one=1;setsockopt(broker,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(broker,(void*)&a,sizeof(a)));assert(!listen(broker,8));fcntl(broker,F_SETFL,O_NONBLOCK);
    socklen_t size=sizeof(a);assert(!getsockname(broker,(void*)&a,&size));broker_port=ntohs(a.sin_port);
    messaging();failures();
#ifdef ASTER_MQTT_TLS_TEST
    tls_admission();
#endif
    close(broker);free(worker_stack);free(http_stack);
    puts("MQTT framing, QoS, reconnect, heartbeat, cancellation and 100 lifecycle recovery OK");
    return 0;
}
