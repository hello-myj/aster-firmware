/* SPDX-License-Identifier: MIT
 * One bounded client, using the existing HTTP/MQTT worker and transport slots.
 */
#include "aster_ws.h"
#include "aster_http.h"
#include "aster_mem.h"
#include "aster_net.h"
#if defined(CONFIG_ASTER_HTTP_TLS)
#include "aster_http_tls.h"
#endif
#include <errno.h>
#include <string.h>
#ifndef ASTER_WS_HOST_TEST
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#endif
struct ws_client {
    uint64_t owner, transport_owner;
    uint32_t handle, transport;
    bool closing, cancel, stopped, peer_close, close_sent, sending_message;
    bool upgraded, ping_wait;
    int64_t deadline, close_at, last_rx, ping_at, output_at, fragment_at;
    struct aster_http_url url;
    struct aster_ws_snapshot pub;
    char expected_accept[29];
    char *ca; size_t ca_length;
#if defined(CONFIG_ASTER_HTTP_TLS)
    struct aster_http_tls *tls;
    struct aster_http_tls_report tls_report;
#endif
    uint8_t input[ASTER_WS_WIRE_MAX], output[ASTER_WS_WIRE_MAX];
    uint8_t assembly[ASTER_WS_MESSAGE_MAX], message[ASTER_WS_MESSAGE_MAX];
    uint8_t control[131], heartbeat[4];
    size_t input_length, output_length, output_offset, control_length, assembly_length, message_length;
    unsigned assembly_opcode, message_opcode;
};
static struct ws_client *ws_client;
static uint32_t ws_generation;
K_MUTEX_DEFINE(ws_lock);
static struct ws_client *ws_find(uint64_t owner,uint32_t handle)
{return ws_client&&ws_client->owner==owner&&ws_client->handle==handle&&!ws_client->closing?ws_client:NULL;}
static int ws_random(void *data,size_t length) { return sys_csrand_get(data,length)?-EIO:0; }
static int ws_encode(uint8_t *out,size_t cap,unsigned op,const void *p,size_t n)
{uint8_t mask[4];int rc=ws_random(mask,sizeof(mask));return rc?rc:aster_ws_encode(out,cap,op,true,p,n,mask);}
static int ws_tls_drop(struct ws_client *c)
{
#if defined(CONFIG_ASTER_HTTP_TLS)
    return aster_http_tls_free(&c->tls);
#else
    (void)c;return 0;
#endif
}
static int ws_send(struct ws_client *c,const void *p,size_t n)
{
#if defined(CONFIG_ASTER_HTTP_TLS)
    if(c->tls)return aster_http_tls_send(c->tls,p,n,&c->tls_report);
#endif
    return aster_net_send(c->transport_owner,c->transport,p,n,NULL,0);
}
static int ws_recv(struct ws_client *c,void *p,size_t n)
{
#if defined(CONFIG_ASTER_HTTP_TLS)
    if(c->tls)return aster_http_tls_recv(c->tls,p,n,&c->tls_report);
#endif
    size_t required;return aster_net_recv(c->transport_owner,c->transport,p,n,&required,NULL,NULL);
}
static void ws_fault(struct ws_client *c,int error)
{c->pub.error=error;c->pub.state=c->cancel?ASTER_WS_CANCELLED:ASTER_WS_FAILED;c->control_length=0;c->output_length=c->output_offset=0;c->sending_message=false;}
static int ws_flush(struct ws_client *c)
{
    if(c->output_offset==c->output_length){c->output_offset=c->output_length=0;return 0;}
    if(k_uptime_get()-c->output_at>=10000)return -ETIMEDOUT;
    size_t n=c->output_length-c->output_offset;if(n>ASTER_NET_TCP_BUFFER)n=ASTER_NET_TCP_BUFFER;
    int rc=ws_send(c,c->output+c->output_offset,n);
    if(rc<0)return rc;
    if(!rc)return -EAGAIN;
    c->output_offset+=rc;
    if(c->output_offset!=c->output_length)return -EAGAIN;
    c->output_offset=c->output_length=0;
    if(c->sending_message){c->sending_message=false;c->pub.sent++;}
    return 0;
}
static int ws_control(struct ws_client *c,unsigned op,const void *p,size_t n)
{
    if(c->control_length)return -EAGAIN;
    int rc=ws_encode(c->control,sizeof(c->control),op,p,n);
    if(rc<0)return rc;
    c->control_length=rc;return 0;
}
static int ws_frame(struct ws_client *c,const struct aster_ws_frame *f)
{
    if(f->opcode==9){int rc=ws_control(c,10,f->data,f->length);if(!rc)c->pub.pings++;return rc;}
    if(f->opcode==10){c->pub.pongs++;if(c->ping_wait&&f->length==sizeof(c->heartbeat)&&!memcmp(f->data,c->heartbeat,sizeof(c->heartbeat)))c->ping_wait=false;return 0;}
    if(f->opcode==8){
        int rc=aster_ws_close_check(f->data,f->length);if(rc)return rc;
        if(!c->close_sent){rc=ws_control(c,8,f->data,f->length);if(rc)return rc;c->close_sent=true;}
        c->peer_close=true;c->pub.close_code=f->length?((unsigned)f->data[0]<<8)|f->data[1]:1005;
        c->pub.state=ASTER_WS_CLOSING;if(!c->close_at)c->close_at=k_uptime_get()+1500;return 0;
    }
    if(c->pub.state!=ASTER_WS_READY)return -EPROTO;
    if((!f->opcode&&!c->assembly_opcode)||(f->opcode&&c->assembly_opcode))return -EPROTO;
    if(f->length>ASTER_WS_MESSAGE_MAX-c->assembly_length)return -EMSGSIZE;
    if(f->fin&&c->message_opcode)return -EAGAIN;
    if(f->opcode){c->assembly_opcode=f->opcode;c->fragment_at=k_uptime_get();}
    memcpy(c->assembly+c->assembly_length,f->data,f->length);c->assembly_length+=f->length;
    if(f->fin){
        if(c->assembly_opcode==1&&!aster_ws_utf8(c->assembly,c->assembly_length))return -EILSEQ;
        memcpy(c->message,c->assembly,c->assembly_length);c->message_length=c->assembly_length;
        c->message_opcode=c->assembly_opcode;c->assembly_opcode=0;c->assembly_length=0;c->pub.received++;
    }
    return 0;
}
static int ws_read_frame(struct ws_client *c)
{
    struct aster_ws_frame f;int rc=aster_ws_decode(c->input,c->input_length,&f);
    if(!rc&&c->input_length<sizeof(c->input)){
        size_t cap=sizeof(c->input)-c->input_length;if(cap>1024)cap=1024;
        int n=ws_recv(c,c->input+c->input_length,cap);
        if(n==-EAGAIN)return 0;
        if(n<=0)return n?n:-ECONNRESET;
        c->input_length+=n;c->last_rx=k_uptime_get();rc=aster_ws_decode(c->input,c->input_length,&f);
    }
    if(rc<=0)return rc;
    rc=ws_frame(c,&f);if(rc==-EAGAIN)return 0;if(rc)return rc;
    c->input_length-=f.wire_length;memmove(c->input,c->input+f.wire_length,c->input_length);return 0;
}
static bool ws_stopped(struct ws_client *c)
{
    if(c->stopped)return true;
    if(ws_tls_drop(c)==-EAGAIN)return false;
    if(c->transport){int rc=aster_net_finish(c->transport_owner,c->transport);
        if(rc==-EAGAIN)return false;
        if(rc==-EINVAL){aster_net_cancel(c->transport_owner,c->transport);return false;}}
    c->stopped=true;return true;
}
static void ws_process(struct ws_client *c)
{
    int64_t now=k_uptime_get();int rc=0;
    if(c->cancel&&c->pub.state<ASTER_WS_CLOSED)ws_fault(c,-ECANCELED);
    if(c->pub.state>=ASTER_WS_CLOSED){ws_stopped(c);return;}
    if(c->pub.state<ASTER_WS_READY&&now>=c->deadline){ws_fault(c,-ETIMEDOUT);return;}
    if(c->pub.state==ASTER_WS_CONNECTING){
        struct aster_net_snapshot t;rc=aster_net_snapshot(c->transport_owner,c->transport,&t);
        if(rc==-EAGAIN)return;
        if(rc||t.state==ASTER_NET_FAILED||t.state==ASTER_NET_EOF){ws_fault(c,rc?rc:t.error?t.error:-ECONNRESET);return;}
        if(t.state!=ASTER_NET_READY)return;
        c->pub.state=c->url.tls?ASTER_WS_TLS:ASTER_WS_UPGRADE;
    }
#if defined(CONFIG_ASTER_HTTP_TLS)
    if(c->pub.state==ASTER_WS_TLS){
        if(!c->tls)rc=aster_http_tls_new(&c->tls,c->transport_owner,c->transport,c->url.host,c->ca,c->ca_length,&c->tls_report);
        if(!rc)rc=aster_http_tls_handshake(c->tls,&c->tls_report);
        c->pub.tls_verify=c->tls_report.verify;
        if(rc==-EAGAIN)return;
        if(rc){ws_fault(c,rc);return;}c->pub.state=ASTER_WS_UPGRADE;
    }
#endif
    if(c->pub.state==ASTER_WS_UPGRADE&&!c->upgraded){
        uint8_t nonce[16];char key[25];rc=ws_random(nonce,sizeof(nonce));
        if(!rc){aster_ws_key(nonce,key,c->expected_accept);rc=aster_ws_request(&c->url,key,(char *)c->output,sizeof(c->output));}
        if(rc<0){ws_fault(c,rc);return;}
        c->output_length=rc;c->output_at=now;c->upgraded=true;
    }
    rc=ws_flush(c);if(rc&&rc!=-EAGAIN){ws_fault(c,rc);return;}
    if(!c->output_length&&c->control_length){
        memcpy(c->output,c->control,c->control_length);c->output_length=c->control_length;
        c->output_at=now;c->control_length=0;
    }
    if(c->pub.state==ASTER_WS_UPGRADE){
        /* Bytewise upgrade consumption leaves the first WebSocket frame in
         * the transport; a coalesced response+frame is never discarded. */
        if(c->output_length)return;
        for(unsigned i=0;i<128;i++){
            if(c->input_length==ASTER_WS_HANDSHAKE_MAX){ws_fault(c,-EMSGSIZE);return;}
            int n=ws_recv(c,c->input+c->input_length,1);
            if(n==-EAGAIN)return;
            if(n<=0){ws_fault(c,n?n:-ECONNRESET);return;}
            c->input_length++;
            if(c->input_length>=4&&!memcmp(c->input+c->input_length-4,"\r\n\r\n",4)){
                rc=aster_ws_response((char *)c->input,c->input_length,c->expected_accept);
                if(rc){ws_fault(c,rc);return;}c->input_length=0;c->pub.state=ASTER_WS_READY;c->last_rx=now;break;
            }
        }
        return;
    }
    if(c->pub.state==ASTER_WS_READY){
        if((c->ping_wait&&now-c->ping_at>=10000)||(c->assembly_opcode&&now-c->fragment_at>=30000)){ws_fault(c,-ETIMEDOUT);return;}
        if(!c->ping_wait&&!c->control_length&&now-c->last_rx>=15000){
            rc=ws_random(c->heartbeat,sizeof(c->heartbeat));if(!rc)rc=ws_control(c,9,c->heartbeat,sizeof(c->heartbeat));
            if(rc){ws_fault(c,rc);return;}c->ping_wait=true;c->ping_at=now;
        }
    }
    if(c->pub.state==ASTER_WS_CLOSING){
        if(now>=c->close_at||(c->peer_close&&!c->output_length&&!c->control_length)){
            struct aster_net_snapshot t;rc=aster_net_snapshot(c->transport_owner,c->transport,&t);
            if(now<c->close_at&&(rc==-EAGAIN||(!rc&&t.tx_queued)))return;
            c->pub.state=ASTER_WS_CLOSED;ws_stopped(c);return;
        }
    }
    if(!c->peer_close){rc=ws_read_frame(c);if(rc)ws_fault(c,rc);}
}
static bool ws_dispose(struct ws_client *c)
{
    int64_t now=k_uptime_get();if(!c->close_at)c->close_at=now+1500;
    if(!c->cancel&&c->pub.state==ASTER_WS_READY&&now<c->close_at){
        if(!c->close_sent){const uint8_t normal[]={3,232};int rc=ws_control(c,8,normal,sizeof(normal));if(rc==-EAGAIN)return false;if(!rc){c->close_sent=true;c->pub.state=ASTER_WS_CLOSING;}}
    }
    if(!c->cancel&&c->pub.state==ASTER_WS_CLOSING&&now<c->close_at){ws_process(c);if(c->pub.state==ASTER_WS_CLOSING)return false;}
    if(ws_tls_drop(c)==-EAGAIN)return false;
    if(c->transport&&aster_net_close(c->transport_owner,c->transport)==-EAGAIN)return false;
    if(c->ca&&aster_http_svc_tls_release(&c->ca)==-EAGAIN)return false;
    return true;
}
bool aster_ws_svc_tick(void)
{
    k_mutex_lock(&ws_lock,K_FOREVER);bool active=ws_client!=NULL;
    if(ws_client){if(ws_client->closing){if(ws_dispose(ws_client)){aster_psram_free(ws_client);ws_client=NULL;}}else ws_process(ws_client);}
    k_mutex_unlock(&ws_lock);return active;
}
int aster_ws_svc_open(uint64_t owner,const char *url,uint32_t timeout)
{
    if(!owner||owner>INT64_MAX||timeout<100||timeout>30000)return -EINVAL;
    struct aster_http_url parsed;int rc=aster_ws_url_parse(url,&parsed);if(rc)return rc;
#if !defined(CONFIG_ASTER_HTTP_TLS)
    if(parsed.tls)return -ENOTSUP;
#endif
    rc=aster_http_svc_worker_wake();if(rc)return rc;
    if(k_mutex_lock(&ws_lock,K_NO_WAIT))return -EAGAIN;
    if(ws_client){rc=-ENOSPC;goto done;}
    if(ws_generation==INT32_MAX){rc=-EOVERFLOW;goto done;}
    struct ws_client *c=aster_psram_alloc(sizeof(*c));if(!c){rc=-ENOMEM;goto done;}
    memset(c,0,sizeof(*c));c->owner=owner;c->url=parsed;c->transport_owner=UINT64_C(0xa000000000000000)|++ws_generation;
    if(parsed.tls){rc=aster_http_svc_tls_acquire(&c->ca,&c->ca_length);if(rc){aster_psram_free(c);goto done;}}
    rc=aster_net_open(c->transport_owner,ASTER_NET_TCP,c->url.host,c->url.port,timeout);
    if(rc<0){c->closing=c->cancel=true;ws_client=c;goto done;}
    c->handle=c->transport=rc;c->pub.state=ASTER_WS_CONNECTING;c->deadline=k_uptime_get()+timeout;ws_client=c;
done:
    k_mutex_unlock(&ws_lock);aster_http_svc_worker_signal();return rc;
}
int aster_ws_svc_send(uint64_t owner,uint32_t handle,unsigned opcode,const void *data,size_t n)
{
    if(opcode!=1&&opcode!=2)return -EINVAL;
    if(n>ASTER_WS_MESSAGE_MAX)return -EMSGSIZE;
    if((!data&&n)||(opcode==1&&!aster_ws_utf8(data,n)))return -EINVAL;
    if(k_mutex_lock(&ws_lock,K_NO_WAIT))return -EAGAIN;
    struct ws_client *c=ws_find(owner,handle);int rc=-EBADF;
    if(c){rc=c->pub.state!=ASTER_WS_READY?-ENOTCONN:c->output_length||c->control_length?-EAGAIN:0;
        if(!rc){rc=ws_encode(c->output,sizeof(c->output),opcode,data,n);if(rc>0){c->output_length=rc;c->output_at=k_uptime_get();c->sending_message=true;rc=0;}}}
    k_mutex_unlock(&ws_lock);return rc;
}
int aster_ws_svc_status(uint64_t owner,uint32_t handle,struct aster_ws_snapshot *out)
{
    if(!out)return -EINVAL;
    if(k_mutex_lock(&ws_lock,K_NO_WAIT))return -EAGAIN;
    struct ws_client *c=ws_find(owner,handle);if(c){*out=c->pub;out->queued=!!c->message_opcode;out->pending=!!c->sending_message;
#if defined(CONFIG_ASTER_HTTP_TLS)
        out->tls_error=c->tls_report.error;out->transport_error=c->tls_report.transport_error;
#endif
    }k_mutex_unlock(&ws_lock);return c?0:-EBADF;
}
int aster_ws_svc_recv(uint64_t owner,uint32_t handle,void *data,size_t capacity,size_t *length,unsigned *opcode)
{
    if(!length||!opcode||(!data&&capacity))return -EINVAL;
    if(k_mutex_lock(&ws_lock,K_NO_WAIT))return -EAGAIN;
    struct ws_client *c=ws_find(owner,handle);int rc=-EBADF;
    if(c){if(c->message_opcode){*length=c->message_length;*opcode=c->message_opcode;rc=-EMSGSIZE;
            if(capacity>=c->message_length){if(c->message_length)memcpy(data,c->message,c->message_length);c->message_opcode=0;rc=0;}}
        else rc=c->pub.state==ASTER_WS_CLOSED?-ENODATA:c->pub.state>=ASTER_WS_FAILED?c->pub.error:-EAGAIN;}
    k_mutex_unlock(&ws_lock);return rc;
}
static int ws_end(uint64_t owner,uint32_t handle,bool close)
{
    if(k_mutex_lock(&ws_lock,K_NO_WAIT))return -EAGAIN;
    struct ws_client *c=ws_find(owner,handle);if(c){if(close)c->closing=true;else c->cancel=true;}
    k_mutex_unlock(&ws_lock);aster_http_svc_worker_signal();return c?0:-EBADF;
}
int aster_ws_svc_close(uint64_t owner,uint32_t h){return ws_end(owner,h,true);}
int aster_ws_svc_cancel(uint64_t owner,uint32_t h){return ws_end(owner,h,false);}
int aster_ws_svc_close_owner(uint64_t owner)
{
    if(k_mutex_lock(&ws_lock,K_NO_WAIT))return -EAGAIN;
    if(ws_client&&ws_client->owner==owner)ws_client->closing=true;
    k_mutex_unlock(&ws_lock);return 0;
}
int aster_ws_svc_stats(struct aster_ws_stats *out)
{
    if(!out)return -EINVAL;
    if(k_mutex_lock(&ws_lock,K_NO_WAIT))return -EAGAIN;
    memset(out,0,sizeof(*out));if(ws_client){out->active=!ws_client->closing;out->closing=ws_client->closing;out->tls_active=!!ws_client->ca;out->psram_bytes=sizeof(*ws_client)+ws_client->ca_length;
#if defined(CONFIG_ASTER_HTTP_TLS)
        if(ws_client->tls)out->psram_bytes+=aster_http_tls_context_bytes();
#endif
    }k_mutex_unlock(&ws_lock);return 0;
}
