/* SPDX-License-Identifier: MIT */
#include "aster_ws_cmd.h"
#include "aster_ws.h"
#include "aster_mem.h"
#include <zephyr/sys/crc.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#define OWNER ASTER_WS_CLI_OWNER
static int ws_number(const char *p,uint32_t max,uint32_t *out)
{
    if(!p||!*p||*p=='-'||*p=='+')return -EINVAL;
    char *end;errno=0;unsigned long n=strtoul(p,&end,10);
    if(errno||*end||n>max)return -EINVAL;
    *out=n;return 0;
}
static int ws_reply(const struct shell *sh,int rc){shell_print(sh,"WS result=%d",rc);return 0;}
static int ws_open_cmd(const struct shell *sh,size_t argc,char **argv)
{
    uint32_t timeout=15000;
    if(argc>2&&ws_number(argv[2],30000,&timeout))return ws_reply(sh,-EINVAL);
    int rc=aster_ws_svc_open(OWNER,argv[1],timeout);
    if(rc>0)shell_print(sh,"WS handle=%d",rc);else ws_reply(sh,rc);
    return 0;
}
static int ws_status_cmd(const struct shell *sh,size_t argc,char **argv)
{
    (void)argc;uint32_t h;if(ws_number(argv[1],INT32_MAX,&h)||!h)return ws_reply(sh,-EINVAL);
    struct aster_ws_snapshot s;int rc=aster_ws_svc_status(OWNER,h,&s);if(rc)return ws_reply(sh,rc);
    shell_print(sh,"WS handle=%u state=%u error=%d queued=%u pending=%u received=%u sent=%u pings=%u pongs=%u close_code=%u verify=%u tls_error=%d transport_error=%d",h,s.state,s.error,s.queued,s.pending,s.received,s.sent,s.pings,s.pongs,s.close_code,s.tls_verify,s.tls_error,s.transport_error);return 0;
}
static int ws_send_cmd(const struct shell *sh,size_t argc,char **argv)
{
    (void)argc;uint32_t h,op;if(ws_number(argv[1],INT32_MAX,&h)||!h||ws_number(argv[2],2,&op)||!op)return ws_reply(sh,-EINVAL);
    size_t n=!strcmp(argv[3],"-")?0:strlen(argv[3]);if(n%2||n/2>ASTER_WS_MESSAGE_MAX)return ws_reply(sh,-EMSGSIZE);
    uint8_t *data=aster_psram_alloc(n/2+1);if(!data)return ws_reply(sh,-ENOMEM);
    int rc=0;
    for(size_t i=0;i<n/2;i++){unsigned v=0;for(unsigned j=0;j<2;j++){unsigned char c=argv[3][2*i+j];unsigned x=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;if(x>15){rc=-EINVAL;break;}v=(v<<4)|x;}if(rc)break;data[i]=v;}
    if(!rc)rc=aster_ws_svc_send(OWNER,h,op,data,n/2);
    aster_psram_free(data);return ws_reply(sh,rc);
}
static int ws_pattern_cmd(const struct shell *sh,size_t argc,char **argv)
{
    (void)argc;uint32_t h,n,seed;
    if(ws_number(argv[1],INT32_MAX,&h)||!h||ws_number(argv[2],ASTER_WS_MESSAGE_MAX,&n)||ws_number(argv[3],255,&seed))return ws_reply(sh,-EINVAL);
    uint8_t *data=aster_psram_alloc(n?n:1);if(!data)return ws_reply(sh,-ENOMEM);
    for(uint32_t i=0;i<n;i++)data[i]=(uint8_t)((i+seed)%251);
    int rc=aster_ws_svc_send(OWNER,h,2,data,n);aster_psram_free(data);return ws_reply(sh,rc);
}
static int ws_recv_cmd(const struct shell *sh,size_t argc,char **argv)
{
    uint32_t h,cap=ASTER_WS_MESSAGE_MAX;if(ws_number(argv[1],INT32_MAX,&h)||!h||(argc>2&&ws_number(argv[2],ASTER_WS_MESSAGE_MAX,&cap)))return ws_reply(sh,-EINVAL);
    uint8_t *data=aster_psram_alloc(cap?cap:1);if(!data)return ws_reply(sh,-ENOMEM);
    size_t n=0;unsigned op=0;int rc=aster_ws_svc_recv(OWNER,h,data,cap,&n,&op);
    shell_print(sh,"WS handle=%u result=%d opcode=%u required=%zu",h,rc,op,n);
    if(!rc){shell_print(sh,"WS message length=%zu crc32=%08x",n,crc32_ieee(data,n));
        for(size_t i=0;i<n;i+=32){char hex[65];size_t amount=n-i;if(amount>32)amount=32;static const char digits[]="0123456789abcdef";for(size_t j=0;j<amount;j++){hex[2*j]=digits[data[i+j]>>4];hex[2*j+1]=digits[data[i+j]&15];}hex[2*amount]=0;shell_print(sh,"WS data offset=%zu hex=%s",i,hex);}}
    aster_psram_free(data);return 0;
}
static int ws_end_cmd(const struct shell *sh,size_t argc,char **argv)
{
    (void)argc;uint32_t h;if(ws_number(argv[1],INT32_MAX,&h)||!h)return ws_reply(sh,-EINVAL);
    return ws_reply(sh,!strcmp(argv[0],"cancel")?aster_ws_svc_cancel(OWNER,h):aster_ws_svc_close(OWNER,h));
}
static int ws_stats_cmd(const struct shell *sh,size_t argc,char **argv)
{
    (void)argc;(void)argv;struct aster_ws_stats s;int rc=aster_ws_svc_stats(&s);if(rc)return ws_reply(sh,rc);
    shell_print(sh,"WS active=%u closing=%u tls=%u psram=%u",s.active,s.closing,s.tls_active,s.psram_bytes);return 0;
}
static const struct shell_static_entry ws_entries[] = {
    SHELL_CMD_ARG(open,NULL,"ws(s)://HOST[:PORT]/PATH [TIMEOUT_MS]",ws_open_cmd,2,1),
    SHELL_CMD_ARG(status,NULL,"HANDLE",ws_status_cmd,2,0),
    SHELL_CMD_ARG(send,NULL,"HANDLE OPCODE(1=text,2=binary) HEX|-",ws_send_cmd,4,0),
    SHELL_CMD_ARG(pattern,NULL,"HANDLE LENGTH SEED (bounded binary test payload)",ws_pattern_cmd,4,0),
    SHELL_CMD_ARG(recv,NULL,"HANDLE [CAPACITY]",ws_recv_cmd,2,1),
    SHELL_CMD_ARG(cancel,NULL,"HANDLE",ws_end_cmd,2,0),
    SHELL_CMD_ARG(close,NULL,"HANDLE",ws_end_cmd,2,0),
    SHELL_CMD(stats,NULL,"bounded WebSocket service",ws_stats_cmd),SHELL_SUBCMD_SET_END};
const union shell_cmd_entry aster_ws_cmds={.entry=ws_entries};
