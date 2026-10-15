/* SPDX-License-Identifier: MIT
 * Host harness for the actual service: real POSIX sockets, deterministic DNS
 * and clock, manually stepped worker. No reimplementation of service logic.
 */
#ifndef NET_HOST_PLATFORM_H
#define NET_HOST_PLATFORM_H
#include <assert.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
struct net_if { int lock; };
static struct net_if host_iface;
static int host_if_missing, host_if_no_address;
#define NET_ADDR_PREFERRED 1
static struct net_if *net_if_get_default(void) {return host_if_missing ? NULL : &host_iface;}
static int net_if_is_up(struct net_if *iface) {(void)iface;return 1;}
static int net_if_is_carrier_ok(struct net_if *iface) {(void)iface;return 1;}
static const struct in_addr *net_if_ipv4_get_global_addr(struct net_if *iface,int state)
{(void)iface;(void)state;static struct in_addr addr;addr.s_addr=htonl(INADDR_LOOPBACK);return host_if_no_address?NULL:&addr;}
#define zsock_socket socket
#define zsock_close close
#define zsock_bind bind
#define zsock_connect connect
static size_t host_recv_limit;
static ssize_t host_recv(int fd,void *data,size_t length,int flags)
{
    if(host_recv_limit&&length>host_recv_limit)length=host_recv_limit;
    return recv(fd,data,length,flags);
}
#define zsock_recv host_recv
#define zsock_send host_send
#define zsock_sendto sendto
#define zsock_recvfrom recvfrom
#define zsock_getsockopt getsockopt
#define zsock_getsockname getsockname
#define zsock_fcntl fcntl
#define zsock_inet_pton inet_pton
#define zsock_poll poll
#define zsock_pollfd pollfd
#define ZSOCK_POLLOUT POLLOUT
#define K_MUTEX_DEFINE(n) static int n
#define K_SEM_DEFINE(n,a,b) static int n
#define K_FOREVER -1
#define K_NO_WAIT 0
#define K_MSEC(n) (n)
#define K_PRIO_PREEMPT(n) (n)
#define K_KERNEL_STACK_LEN(n) (n)
typedef char k_thread_stack_t;
struct k_thread { int dummy; };
static int64_t host_now;
static int64_t k_uptime_get(void) { return host_now; }
static int k_mutex_lock(int *m, int timeout) { if (*m && timeout == K_NO_WAIT) return -EBUSY; assert(!*m); *m=1; return 0; }
static void k_mutex_unlock(int *m) { assert(*m); *m=0; }
static void k_sem_give(int *s) { (void)s; }
static void k_sem_take(int *s, int t) { (void)s; (void)t; }
static void k_thread_create(struct k_thread *t, char *s, size_t n,
    void (*fn)(void *,void *,void *), void *a,void *b,void *c,int p,int o,int d)
{ (void)t;(void)s;(void)n;(void)fn;(void)a;(void)b;(void)c;(void)p;(void)o;(void)d; }
static void k_thread_name_set(struct k_thread *t, const char *n) { (void)t;(void)n; }
enum dns_resolve_status { DNS_EAI_INPROGRESS=-100, DNS_EAI_ALLDONE=-103, DNS_EAI_CANCELED=-101 };
struct dns_addrinfo { int ai_family; struct sockaddr ai_addr; };
#define DNS_QUERY_TYPE_A 1
static void (*dns_cb)(enum dns_resolve_status,struct dns_addrinfo *,void *);
static void *dns_token;
static int dns_mode, dns_active;
static int dns_get_addr_info(const char *h,int type,uint16_t *id,
 void (*cb)(enum dns_resolve_status,struct dns_addrinfo *,void *),void *token,int timeout)
{
 (void)h;(void)type;assert(timeout>0);*id=42;dns_cb=cb;dns_token=token;dns_active=1;
 if(dns_mode==1) {
  struct dns_addrinfo a={.ai_family=AF_INET};
  ((struct sockaddr_in *)&a.ai_addr)->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  cb(DNS_EAI_INPROGRESS,&a,token);cb(DNS_EAI_ALLDONE,NULL,token);dns_active=0;
 } else if(dns_mode==2) { cb(DNS_EAI_ALLDONE,NULL,token);dns_active=0; }
 return 0;
}
static int dns_cancel_addr_info_with_name(const char *h,int type,uint16_t id)
{
 (void)h;(void)type;(void)id;
 if(dns_active){dns_active=0;dns_cb(DNS_EAI_CANCELED,NULL,dns_token);}
 return 0;
}
static int send_blocked;
static ssize_t host_send(int fd, const void *data, size_t n, int flags)
{
 if (send_blocked) { errno=EAGAIN; return -1; }
 return send(fd,data,n,flags);
}
static size_t psram_live;
static int fail_alloc, fail_dram;
void *aster_psram_alloc(size_t n) { if(fail_alloc)return NULL;void *p=malloc(n+sizeof(size_t)); if(!p)return NULL;*(size_t *)p=n;psram_live+=n;return (size_t *)p+1; }
void aster_psram_free(void *p) { if(p){size_t *q=(size_t *)p-1;psram_live-=*q;free(q);} }
void *aster_dram_alloc(size_t n) { return fail_dram ? NULL : malloc(n); }
void aster_dram_free(void *p) { free(p); }
#endif
