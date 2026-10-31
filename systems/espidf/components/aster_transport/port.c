/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/net/dns_resolve.h>
#include "aster_net.h"
#include "wifi_service.h"
#include "esp_netif.h"
#include <lwip/netdb.h>
#include <string.h>
#include <stdlib.h>
#include <psa/crypto.h>
ssize_t aster_idf_recvfrom(int fd,void *data,size_t size,int flags,struct sockaddr *address,socklen_t *length)
{
    struct iovec vector={.iov_base=data,.iov_len=size};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_name=address,.msg_namelen=length?*length:0};
    ssize_t n=recvmsg(fd,&message,flags);
    if(n>=0 && length)*length=message.msg_namelen;
    if(n>=0 && (flags&MSG_TRUNC) && (message.msg_flags&MSG_TRUNC) && (size_t)n<=size)return size+1;
    return n;
}
int aster_idf_secure_random(void *out,size_t size)
{
    if(psa_crypto_init()!=PSA_SUCCESS)return -EIO;
    return psa_generate_random(out,size)==PSA_SUCCESS?0:-EIO;
}
/* Resolver jobs own copied names and numeric tokens only. Cancellation retires
 * the callback immediately; late OS results cannot touch a VM or a new job. */
static K_MUTEX_DEFINE(dns_guard);
static struct {uint16_t id;bool busy,cancelled;char host[254];
    void (*callback)(enum dns_resolve_status,struct dns_addrinfo *,void *);void *token;} jobs[2];
static uint16_t generation;
static void resolver(void *arg)
{
    unsigned index=(unsigned)(uintptr_t)arg;
    struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM},*address=NULL;
    int result=getaddrinfo(jobs[index].host,NULL,&hints,&address);
    k_mutex_lock(&dns_guard,K_FOREVER);
    if(!jobs[index].cancelled) {
        if(!result && address && address->ai_addrlen>=sizeof(struct sockaddr_in)) {
            struct dns_addrinfo out={.ai_family=AF_INET};memcpy(&out.ai_addr,address->ai_addr,sizeof(struct sockaddr_in));
            jobs[index].callback(DNS_EAI_INPROGRESS,&out,jobs[index].token);
        }
        jobs[index].callback(DNS_EAI_ALLDONE,NULL,jobs[index].token);
    }
    jobs[index].busy=false;k_mutex_unlock(&dns_guard);
    if(address)freeaddrinfo(address);
    vTaskDelete(NULL);
}
int dns_get_addr_info(const char *host,int type,uint16_t *id,
    void (*callback)(enum dns_resolve_status,struct dns_addrinfo *,void *),void *token,int timeout)
{
    if(type!=DNS_QUERY_TYPE_A || !host || strlen(host)>253 || !id || !callback || timeout<=0)return -EINVAL;
    k_mutex_lock(&dns_guard,K_FOREVER);int index=-1;
    for(int i=0;i<2;i++)if(!jobs[i].busy){index=i;break;}
    if(index<0){k_mutex_unlock(&dns_guard);return -EAGAIN;}
    do {if(!++generation)++generation;}
    while((jobs[0].busy && jobs[0].id==generation) || (jobs[1].busy && jobs[1].id==generation));
    jobs[index].id=*id=generation;jobs[index].busy=true;jobs[index].cancelled=false;
    strcpy(jobs[index].host,host);jobs[index].callback=callback;jobs[index].token=token;
    if(xTaskCreate(resolver,"aster_dns",4096,(void *)(uintptr_t)index,3,NULL)!=pdPASS) {
        jobs[index].busy=false;k_mutex_unlock(&dns_guard);return -ENOMEM;
    }
    k_mutex_unlock(&dns_guard);return 0;
}
int dns_cancel_addr_info_with_name(const char *host,int type,uint16_t id)
{
    (void)host;(void)type;k_mutex_lock(&dns_guard,K_FOREVER);
    for(unsigned i=0;i<2;i++)if(jobs[i].busy && jobs[i].id==id && !jobs[i].cancelled) {
        jobs[i].cancelled=true;jobs[i].callback(DNS_EAI_CANCELED,NULL,jobs[i].token);
    }
    k_mutex_unlock(&dns_guard);return 0;
}
int aster_idf_net_interface_info(struct aster_net_interface_info *out)
{
    if(!out)return -EINVAL;
    *out=(struct aster_net_interface_info){0};
    esp_netif_t *iface=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");if(!iface)return 0;
    out->flags=1;if(esp_netif_is_netif_up(iface))out->flags|=2;
    aster_wifi_info_t wifi;if(!aster_idf_wifi_snapshot(&wifi) && wifi.state==ASTER_WIFI_STATE_COMPLETED)out->flags|=4;
    esp_netif_ip_info_t ip;
    if(esp_netif_get_ip_info(iface,&ip)==ESP_OK && ip.ip.addr){out->ipv4=ip.ip.addr;out->flags|=8;}
    return 0;
}
