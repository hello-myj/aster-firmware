/* SPDX-License-Identifier: MIT
 * Bounded RFC 6455 wire codec. SHA-1 here serves only the public upgrade proof,
 * never TLS, signatures, credential hashing or random-number generation.
 */
#include "aster_ws_codec.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
static uint32_t rotate(uint32_t v, unsigned n) { return (v << n) | (v >> (32u-n)); }
static void accept_digest(const char key[25], uint8_t digest[20])
{
    /* The nonce is exactly 24 base64 bytes plus the fixed 36-byte GUID. */
    uint8_t bytes[128] = {0}; uint32_t hash[5] = {0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    memcpy(bytes,key,24); memcpy(bytes+24,"258EAFA5-E914-47DA-95CA-C5AB0DC85B11",36);
    bytes[60]=0x80; bytes[126]=1; bytes[127]=0xe0; /* 60*8 bits */
    for (unsigned block=0;block<2;block++) {
        uint32_t w[80];
        for(unsigned i=0;i<16;i++){const uint8_t *p=bytes+block*64+i*4;w[i]=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
        for(unsigned i=16;i<80;i++)w[i]=rotate(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        uint32_t a=hash[0],b=hash[1],c=hash[2],d=hash[3],e=hash[4];
        for(unsigned i=0;i<80;i++) {
            uint32_t f,k;
            if(i<20){f=(b&c)|(~b&d);k=0x5a827999;}
            else if(i<40){f=b^c^d;k=0x6ed9eba1;}
            else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8f1bbcdc;}
            else{f=b^c^d;k=0xca62c1d6;}
            uint32_t t=rotate(a,5)+f+e+k+w[i];e=d;d=c;c=rotate(b,30);b=a;a=t;
        }
        hash[0]+=a;hash[1]+=b;hash[2]+=c;hash[3]+=d;hash[4]+=e;
    }
    for(unsigned i=0;i<20;i++)digest[i]=(uint8_t)(hash[i/4]>>(24-8*(i%4)));
}
static void base64(const uint8_t *p,size_t n,char *out)
{
    static const char table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t j=0;
    for(size_t i=0;i<n;i+=3){uint32_t v=(uint32_t)p[i]<<16; if(i+1<n)v|=(uint32_t)p[i+1]<<8;if(i+2<n)v|=p[i+2];
        out[j++]=table[v>>18];out[j++]=table[(v>>12)&63];out[j++]=i+1<n?table[(v>>6)&63]:'=';out[j++]=i+2<n?table[v&63]:'=';}
    out[j]=0;
}
void aster_ws_key(const uint8_t random[16],char key[25],char accept[29])
{uint8_t digest[20];base64(random,16,key);accept_digest(key,digest);base64(digest,20,accept);}
int aster_ws_url_parse(const char *text,struct aster_http_url *out)
{
    if(!text||!out)return -EINVAL;
    bool tls=!strncmp(text,"wss://",6);size_t prefix=tls?6:5;
    if(!tls&&strncmp(text,"ws://",5))return -EINVAL;
    size_t n=strnlen(text,ASTER_HTTP_URL_MAX+1);
    if(n>ASTER_HTTP_URL_MAX)return -EMSGSIZE;
    if(strchr(text,'#'))return -EINVAL;
    char temporary[ASTER_HTTP_URL_MAX+4];
    memcpy(temporary,tls?"https://":"http://",prefix+2);memcpy(temporary+prefix+2,text+prefix,n-prefix+1);
    return aster_http_url_parse(temporary,out);
}
int aster_ws_request(const struct aster_http_url *u,const char key[25],char *out,size_t cap)
{
    if(!u||!key||!out||strlen(key)!=24)return -EINVAL;
    int n=snprintf(out,cap,"GET %s HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",u->path,u->host,u->port,key);
    return n<0?-EINVAL:(size_t)n>=cap?-EMSGSIZE:n;
}
static bool token(const char *p,size_t n,const char *want)
{
    size_t cursor=0,wn=strlen(want);
    while(cursor<n){while(cursor<n&&(p[cursor]==' '||p[cursor]=='\t'||p[cursor]==','))cursor++;size_t start=cursor;while(cursor<n&&p[cursor]!=',')cursor++;size_t end=cursor;while(end>start&&(p[end-1]==' '||p[end-1]=='\t'))end--;if(end-start==wn&&!strncasecmp(p+start,want,wn))return true;}
    return false;
}
int aster_ws_response(const char *p,size_t n,const char accept[29])
{
    if(!p||!accept)return -EINVAL;
    if(n>ASTER_WS_HANDSHAKE_MAX)return -EMSGSIZE;
    if(n<19||memcmp(p+n-4,"\r\n\r\n",4))return -EPROTO;
    for(size_t i=0;i<n;i++)if(!p[i]||(unsigned char)p[i]==127||((unsigned char)p[i]<32&&p[i]!='\r'&&p[i]!='\n'&&p[i]!='\t'))return -EPROTO;
    if(strncmp(p,"HTTP/1.1 101 ",13))return -EPROTO;
    unsigned seen=0;size_t pos=0;
    while(pos+1<n&&!(p[pos]=='\r'&&p[pos+1]=='\n')){if(p[pos]=='\n'||p[pos]=='\r')return -EPROTO;pos++;}
    pos+=2;
    while(pos<n-2){size_t end=pos;while(end+1<n&&!(p[end]=='\r'&&p[end+1]=='\n')){if(p[end]=='\r'||p[end]=='\n')return -EPROTO;end++;}
        if(end+1>=n||p[pos]==' '||p[pos]=='\t')return -EPROTO;
        size_t colon=pos;while(colon<end&&p[colon]!=':'){char ch=p[colon];if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||strchr("!#$%&'*+-.^_`|~",ch)))return -EPROTO;colon++;}
        if(colon==pos||colon==end)return -EPROTO;
        size_t start=colon+1,last=end;while(start<last&&(p[start]==' '||p[start]=='\t'))start++;while(last>start&&(p[last-1]==' '||p[last-1]=='\t'))last--;
        const char *v=p+start;size_t len=last-start,field=colon-pos;unsigned bit=0;
        if(field==7&&!strncasecmp(p+pos,"Upgrade",7)){bit=1;if(!token(v,len,"websocket"))return -EPROTO;}
        else if(field==10&&!strncasecmp(p+pos,"Connection",10)){bit=2;if(!token(v,len,"upgrade"))return -EPROTO;}
        else if(field==20&&!strncasecmp(p+pos,"Sec-WebSocket-Accept",20)){bit=4;if(len!=28||memcmp(v,accept,28))return -EPROTO;}
        else if((field==24&&!strncasecmp(p+pos,"Sec-WebSocket-Extensions",24))||(field==22&&!strncasecmp(p+pos,"Sec-WebSocket-Protocol",22)))return -EPROTO;
        if(bit&&(seen&bit))return -EPROTO;
        seen|=bit;pos=end+2;
    }
    return seen==7?0:-EPROTO;
}
int aster_ws_decode(const uint8_t *p,size_t n,struct aster_ws_frame *out)
{
    if(!p||!out)return -EINVAL;
    if(n<2)return 0;
    unsigned op=p[0]&15;bool control=(op&8)!=0;size_t h=2;uint64_t len=p[1]&127;
    if((p[0]&0x70)||(p[1]&0x80)||!(op==0||op==1||op==2||op==8||op==9||op==10))return -EPROTO;
    if(control&&(!(p[0]&128)||len>125))return -EPROTO;
    if(len==126){if(n<4)return 0;len=((uint32_t)p[2]<<8)|p[3];h=4;if(len<126)return -EPROTO;}
    else if(len==127){if(n<10)return 0;if(p[2]&128)return -EPROTO;len=0;for(unsigned i=2;i<10;i++)len=(len<<8)|p[i];h=10;if(len<65536)return -EPROTO;}
    if(len>ASTER_WS_MESSAGE_MAX)return -EMSGSIZE;
    if(n<h+(size_t)len)return 0;
    *out=(struct aster_ws_frame){(uint8_t)op,(p[0]&128)!=0,p+h,(size_t)len,h+(size_t)len};return 1;
}
int aster_ws_encode(uint8_t *out,size_t cap,unsigned op,bool fin,const void *data,size_t n,const uint8_t mask[4])
{
    if(!out||!mask||(!data&&n)||!(op==0||op==1||op==2||op==8||op==9||op==10))return -EINVAL;
    if((op&8)&&(!fin||n>125))return -EINVAL;
    if(n>ASTER_WS_MESSAGE_MAX)return -EMSGSIZE;
    size_t h=n<126?6:8;if(cap<h+n)return -EMSGSIZE;
    out[0]=(fin?128:0)|op;out[1]=128|(n<126?n:126);if(n>=126){out[2]=n>>8;out[3]=n;}
    memcpy(out+h-4,mask,4);const uint8_t *p=data;for(size_t i=0;i<n;i++)out[h+i]=p[i]^mask[i%4];return h+n;
}
bool aster_ws_utf8(const void *data,size_t n)
{
    const uint8_t *p=data;size_t i=0;if(!p&&n)return false;
    while(i<n){unsigned c=p[i++],count;uint32_t code,min;
        if(c<128)continue;
        if(c>=0xc2&&c<=0xdf){count=1;code=c&31;min=0x80;}
        else if(c>=0xe0&&c<=0xef){count=2;code=c&15;min=0x800;}
        else if(c>=0xf0&&c<=0xf4){count=3;code=c&7;min=0x10000;}else return false;
        if(count>n-i)return false;
        while(count--){c=p[i++];if((c&0xc0)!=0x80)return false;code=(code<<6)|(c&63);}
        if(code<min||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
    }return true;
}
int aster_ws_close_check(const void *data,size_t n)
{
    if(n==1||n>125||(!data&&n))return -EPROTO;
    if(n){const uint8_t *p=data;unsigned code=((unsigned)p[0]<<8)|p[1];
        if(code<1000||code>=5000||code==1004||code==1005||code==1006||code==1015||(code>=1016&&code<3000))return -EPROTO;
        if(!aster_ws_utf8(p+2,n-2))return -EILSEQ;
    }return 0;
}
