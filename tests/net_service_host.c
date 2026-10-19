/* SPDX-License-Identifier: MIT */
#define ASTER_NET_HOST_TEST
#include "../systems/zephyr/services/aster_net.c"
#include "../systems/zephyr/services/aster_net_event.c"
#include <stdio.h>
static void tick(void) { ++host_now; service_tick(); }
static int udp(void)
{
 int h=aster_net_open(1,ASTER_NET_UDP,NULL,0,5000); assert(h>0);tick();return h;
}
static struct aster_net_snapshot snap(int h)
{
 struct aster_net_snapshot s;assert(!aster_net_snapshot(1,h,&s));return s;
}
static void dispose(int h)
{
 assert(!aster_net_close(1,h));assert(aster_net_close(1,h)==-EBADF);
 assert(aster_net_snapshot(1,h,&(struct aster_net_snapshot){0})==-EBADF);tick();
}
static void test_udp(void)
{
 int h=udp(), dst=udp();uint16_t port=snap(dst).local_port;assert(port);
 uint16_t source_port=snap(h).local_port;
 char data[1300];memset(data,0xa5,sizeof(data));
 assert(aster_net_send(2,h,data,1,"127.0.0.1",port)==-EBADF);
 assert(aster_net_send(1,h,data,1201,"127.0.0.1",port)==-EMSGSIZE);
 assert(aster_net_send(1,h,data,100,"127.0.0.1",port)==100);
 assert(aster_net_send(1,h,NULL,0,"127.0.0.1",port)==0);
 assert(aster_net_send(1,h,data,1,"127.0.0.1",port)==-EAGAIN);
 tick();tick();tick();size_t required;uint32_t ip;uint16_t from;
 assert(aster_net_recv(1,dst,data,10,&required,NULL,NULL)==-EMSGSIZE && required==100);
 assert(snap(dst).rx_queued==2);
 assert(aster_net_recv(1,dst,data,100,&required,&ip,&from)==100 && data[0]==(char)0xa5);
 assert(ip==htonl(INADDR_LOOPBACK) && from==source_port);
 assert(aster_net_recv(1,dst,NULL,0,&required,NULL,NULL)==0 && required==0);
 assert(aster_net_recv(1,dst,data,1,&required,NULL,NULL)==-EAGAIN);
 /* The documented maximum is a complete datagram, never a truncated read. */
 assert(aster_net_send(1,h,data,1200,"127.0.0.1",port)==1200);tick();tick();
 assert(aster_net_recv(1,dst,data,1199,&required,NULL,NULL)==-EMSGSIZE && required==1200);
 assert(aster_net_recv(1,dst,data,1200,&required,NULL,NULL)==1200);
 int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);
 struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 for(int i=0;i<3;i++){assert(sendto(fd,data,1,0,(struct sockaddr *)&a,sizeof(a))==1);tick();}
 assert(snap(dst).rx_queued==2 && snap(dst).dropped==1);
 assert(sendto(fd,data,sizeof(data),0,(struct sockaddr *)&a,sizeof(a))==sizeof(data));tick();
 assert(snap(dst).dropped==2);close(fd);dispose(h);dispose(dst);
}
static void test_tcp(void)
{
 int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
 struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 assert(!bind(listener,(struct sockaddr *)&a,sizeof(a)));assert(!listen(listener,1));
 socklen_t alen=sizeof(a);assert(!getsockname(listener,(struct sockaddr *)&a,&alen));
 dns_mode=1;
 int h=aster_net_open(1,ASTER_NET_TCP,"echo.test",ntohs(a.sin_port),5000);assert(h>0);
 tick();tick();tick();assert(snap(h).state==ASTER_NET_READY);
 int remote=accept(listener,NULL,NULL);assert(remote>=0);
 uint8_t data[4096];for(size_t i=0;i<sizeof(data);i++)data[i]=(uint8_t)i;
 assert(aster_net_send(1,h,data,sizeof(data),NULL,0)==2048);
 assert(aster_net_send(1,h,data,1,NULL,0)==-EAGAIN);tick();
 uint8_t received[4096];assert(recv(remote,received,sizeof(received),0)==2048);
 assert(!memcmp(received,data,2048));assert(snap(h).tx_bytes==2048);
 /* Simulate native small-packet reads: one tick must drain a bounded batch. */
 host_recv_limit=536;
 assert(send(remote,data,2048,0)==2048);tick();
 assert(snap(h).rx_queued==2048);
 size_t batch_required;
 assert(aster_net_recv(1,h,received,2048,&batch_required,NULL,NULL)==2048);
 assert(!memcmp(received,data,2048));host_recv_limit=0;
 assert(send(remote,data,50,0)==50);assert(send(remote,data+50,100,0)==100);
 assert(!shutdown(remote,SHUT_WR));
 for(int i=0;i<5;i++)tick();
 size_t required;
 assert(aster_net_recv(1,h,received,20,&required,NULL,NULL)==20);
 assert(aster_net_recv(1,h,received+20,130,&required,NULL,NULL)==130);
 assert(!memcmp(received,data,150));
 for(int i=0;i<5;i++)tick();
 assert(aster_net_recv(1,h,received,10,&required,NULL,NULL)==0);
 assert(snap(h).state==ASTER_NET_EOF);
 assert(aster_net_recv(1,h,NULL,0,&required,NULL,NULL)==-EINVAL);
 /* The service intentionally disallows writes after EOF on every host:
  * Zephyr 4.4.2 native TCP does not support half-close. */
 assert(aster_net_send(1,h,data,10,NULL,0)==-EPIPE);
 dispose(h);close(remote);
 h=aster_net_open(1,ASTER_NET_TCP,"127.0.0.1",ntohs(a.sin_port),5000);
 tick();tick();remote=accept(listener,NULL,NULL);assert(remote>=0);
 send_blocked=1;assert(aster_net_send(1,h,data,10,NULL,0)==10);
 host_now+=5001;tick();assert(snap(h).error==-ETIMEDOUT);
 send_blocked=0;dispose(h);close(remote);
 /* Cancellation must preserve already buffered input and then report cancel,
  * never a successful EOF. */
 h=aster_net_open(1,ASTER_NET_TCP,"127.0.0.1",ntohs(a.sin_port),5000);
 tick();tick();remote=accept(listener,NULL,NULL);assert(remote>=0);
 assert(send(remote,data,37,0)==37);tick();assert(snap(h).rx_queued==37);
 assert(!aster_net_cancel(1,h));tick();
 assert(aster_net_recv(1,h,received,100,&required,NULL,NULL)==37);
 assert(!memcmp(received,data,37));
 assert(aster_net_recv(1,h,received,100,&required,NULL,NULL)==-ECANCELED);
 dispose(h);close(remote);close(listener);
}
static void test_lifetime(void)
{
 int a=udp(),b=udp();
 assert(aster_net_open(1,ASTER_NET_UDP,NULL,0,5000)==-ENOSPC);
 assert(!aster_net_close(1,a));
 assert(aster_net_open(1,ASTER_NET_UDP,NULL,0,5000)==-ENOSPC);
 tick();int c=udp();assert(c!=a);
 assert(aster_net_cancel(1,a)==-EBADF);
 assert(!aster_net_cancel(1,c));tick();assert(snap(c).state==ASTER_NET_CANCELLED);
 assert(aster_net_send(1,c,NULL,0,"127.0.0.1",1)==-ECANCELED);
 dispose(c);dispose(b);assert(!psram_live);
 for(int i=0;i<200;i++){int h=udp();dispose(h);}assert(!psram_live);
 fail_alloc=1;assert(aster_net_open(1,ASTER_NET_UDP,NULL,0,5000)==-ENOMEM);fail_alloc=0;
}
static void test_dns(void)
{
 dns_mode=0;
 int h=aster_net_open(1,ASTER_NET_TCP,"pending.test",1234,50);assert(h>0);tick();
 assert(dns_active && snap(h).state==ASTER_NET_RESOLVING);
 void *old_token=dns_token;
 assert(!aster_net_cancel(1,h));tick();assert(!dns_active);
 assert(snap(h).state==ASTER_NET_CANCELLED);dispose(h);
 int fresh=udp();resolved(DNS_EAI_ALLDONE,NULL,old_token);assert(snap(fresh).state==ASTER_NET_READY);dispose(fresh);
 h=aster_net_open(1,ASTER_NET_TCP,"pending.test",1234,20);tick();host_now+=21;tick();
 assert(snap(h).error==-ETIMEDOUT && !dns_active);dispose(h);
 dns_mode=2;
 h=aster_net_open(1,ASTER_NET_TCP,"missing.test",1234,100);tick();tick();
 assert(snap(h).state==ASTER_NET_FAILED && snap(h).error==-EDESTADDRREQ);dispose(h);
 h=udp();
 assert(aster_net_send(1,h,NULL,0,"224.0.0.1",10)==0);tick();
 assert(snap(h).error==-EAFNOSUPPORT);dispose(h);
 int a=udp(),b=udp();(void)a;(void)b;
 aster_net_close_owner(2);tick();assert(psram_live);
 aster_net_close_owner(1);tick();assert(!psram_live);
}
int main(void)
{
 assert(aster_net_open(0,ASTER_NET_UDP,NULL,0,1)==-EINVAL);
 assert(aster_net_open(1,ASTER_NET_TCP,"",80,1)==-EINVAL);
 assert(aster_net_open(1,ASTER_NET_UDP,NULL,0,0)==-EINVAL);
 struct aster_net_stats busy_stats;
 net_lock=1;
 assert(aster_net_open(1,ASTER_NET_UDP,NULL,0,1)==-EAGAIN);
 assert(aster_net_get_stats(&busy_stats)==-EAGAIN);
 assert(aster_net_close_owner(1)==-EAGAIN);
 net_lock=0;
 fail_dram=1;
 assert(aster_net_open(1,ASTER_NET_UDP,NULL,0,1)==-ENOMEM && !psram_live && !worker_stack);
 fail_dram=0;
 test_udp();test_tcp();test_lifetime();test_dns();
 struct aster_net_stats s;aster_net_get_stats(&s);
 assert(!s.active && !s.closing && !s.psram_bytes && s.opened==s.closed);
 free(worker_stack);puts("transport lifecycle, framing, DNS and recovery OK");return 0;
}
