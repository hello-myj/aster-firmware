/* SPDX-License-Identifier: MIT */
#include "aster_ws_codec.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    char key[25],accept[29];aster_ws_key((const uint8_t *)"the sample nonce",key,accept);
    assert(!strcmp(key,"dGhlIHNhbXBsZSBub25jZQ=="));assert(!strcmp(accept,"s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    struct aster_http_url u;assert(!aster_ws_url_parse("wss://example.com:17881/x?q=1",&u)&&u.tls&&u.port==17881&&!strcmp(u.path,"/x?q=1"));
    assert(aster_ws_url_parse("https://example.com/",&u)<0);assert(aster_ws_url_parse("ws://user:pass@example.com/",&u)<0);assert(aster_ws_url_parse("ws://example.com/#fragment",&u)<0);
    assert(!aster_ws_url_parse("ws://example.com/",&u));char request[2048];assert(aster_ws_request(&u,key,request,sizeof(request))>0);assert(strstr(request,"Sec-WebSocket-Version: 13\r\n"));assert(aster_ws_request(&u,key,request,10)==-EMSGSIZE);
    char response[512];int n=snprintf(response,sizeof(response),"HTTP/1.1 101 Switching Protocols\r\nuPgRaDe: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",accept);
    assert(!aster_ws_response(response,n,accept));response[9]='2';assert(aster_ws_response(response,n,accept)==-EPROTO);response[9]='1';
    response[n-7]^=1;assert(aster_ws_response(response,n,accept)==-EPROTO);response[n-7]^=1;
    assert(aster_ws_response(response,n-1,accept)==-EPROTO);
    const char *bad="HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n";assert(aster_ws_response(bad,strlen(bad),accept)==-EPROTO);
    uint8_t wire[ASTER_WS_WIRE_MAX],message[4096];memset(message,0x72,sizeof(message));uint8_t mask[]={1,2,3,4};
    for(size_t size=0;size<=4096;size++){
        int length=aster_ws_encode(wire,sizeof(wire),2,true,message,size,mask);assert(length>0);size_t h=size<126?6:8;
        assert((wire[1]&128)&&!memcmp(wire+h-4,mask,4));for(size_t i=0;i<size;i++)assert((wire[h+i]^mask[i%4])==message[i]);
        struct aster_ws_frame f;assert(aster_ws_decode(wire,length,&f)==-EPROTO);
        wire[1]&=127;memmove(wire+h-4,wire+h,size);h-=4;
        for(size_t i=0;i<size;i++)wire[h+i]=message[i];
        for(size_t prefix=0;prefix<h+size;prefix++)assert(!aster_ws_decode(wire,prefix,&f));
        assert(aster_ws_decode(wire,h+size,&f)==1&&f.length==size&&f.opcode==2&&f.fin);
    }
    struct aster_ws_frame f;const uint8_t huge[]={0x82,127,0,0,0,0,0,1,0,0};assert(aster_ws_decode(huge,sizeof(huge),&f)==-EMSGSIZE);
    const uint8_t noncanonical[]={0x82,126,0,125};assert(aster_ws_decode(noncanonical,sizeof(noncanonical),&f)==-EPROTO);
    const uint8_t control_fragment[]={9,0};assert(aster_ws_decode(control_fragment,2,&f)==-EPROTO);
    const uint8_t reserved[]={0xc1,0};assert(aster_ws_decode(reserved,2,&f)==-EPROTO);
    assert(aster_ws_utf8("\xe4\xb8\xad",3));assert(!aster_ws_utf8("\xc0\x80",2));assert(!aster_ws_utf8("\xed\xa0\x80",3));assert(!aster_ws_utf8("\xf4\x90\x80\x80",4));
    assert(!aster_ws_close_check(NULL,0));const uint8_t close_ok[]={3,232};assert(!aster_ws_close_check(close_ok,2));const uint8_t close_bad[]={3,237};assert(aster_ws_close_check(close_bad,2)==-EPROTO);
    puts("WS RFC vector, upgrade, every frame boundary and UTF8 limits OK");return 0;
}
