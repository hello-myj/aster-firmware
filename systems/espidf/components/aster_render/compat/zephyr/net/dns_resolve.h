/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <lwip/sockets.h>
enum dns_resolve_status {DNS_EAI_INPROGRESS=-100,DNS_EAI_ALLDONE=-103,DNS_EAI_CANCELED=-101};
struct dns_addrinfo {int ai_family;struct sockaddr ai_addr;};
#define DNS_QUERY_TYPE_A 1
int dns_get_addr_info(const char *host,int type,uint16_t *id,
    void (*callback)(enum dns_resolve_status,struct dns_addrinfo *,void *),void *token,int timeout);
int dns_cancel_addr_info_with_name(const char *host,int type,uint16_t id);
