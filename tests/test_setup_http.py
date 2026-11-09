"""Exercise real bootstrap HTTP helpers with transient service-lock contention."""
import unittest
from test_waveshare import ROOT, run_c

class SetupHTTP(unittest.TestCase):
    def test_busy_admission_status_headers_close_and_deadlines(self):
        text=(ROOT/'systems/shared/setup/aster_setup.c').read_text()
        helpers=text[text.index('static int headers('):text.index('static int get_json(')]
        run_c(r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#define OWNER UINT64_C(123)
struct aster_http_options {int unused;};
struct aster_http_snapshot {bool headers_ready;int error;};
static uint64_t clock_ms;
static unsigned calls;
static int mode;
static uint64_t k_uptime_get(void){return clock_ms;}
static void k_msleep(unsigned n){clock_ms+=n;}
static int aster_http_svc_start(uint64_t owner,const struct aster_http_options *o){
 assert(owner==OWNER && o);calls++;
 if(mode==1)return -EINVAL;
 if(mode==2)return -EAGAIN;
 return calls==1?-ENOSPC:calls==2?-EAGAIN:calls==3?-EBUSY:7;
}
static int aster_http_svc_status(uint64_t owner,uint32_t h,struct aster_http_snapshot *s){
 assert(owner==OWNER && h==7);calls++;
 if(calls==1)return -EAGAIN;
 s->error=mode==1?-EIO:0;s->headers_ready=mode!=2 && calls>=3;return 0;
}
static int aster_http_svc_close(uint64_t owner,uint32_t h){
 assert(owner==OWNER && h==7);return mode==2 || ++calls<3?-EAGAIN:0;
}
static int aster_http_svc_header(uint64_t owner,uint32_t h,const char *name,unsigned index,char *out,size_t cap,size_t *len){
 assert(owner==OWNER && h==7 && !strcmp(name,"content-range") && index==0 && cap>=14);
 if(++calls<3)return -EAGAIN;strcpy(out,"bytes 1-3/4");*len=strlen(out);return 0;
}
''' + helpers + r'''
int main(void){
 struct aster_http_options options={0};struct aster_http_snapshot s={0};
 calls=0;mode=0;assert(start_request(&options)==7 && calls==4);
 calls=0;mode=1;assert(start_request(&options)==-EINVAL && calls==1);
 calls=0;mode=2;clock_ms=0;assert(start_request(&options)==-EAGAIN && clock_ms>=5000);
 calls=0;mode=0;assert(!headers(7,&s) && calls==3 && s.headers_ready);
 calls=0;mode=1;assert(headers(7,&s)==-EIO);
 calls=0;mode=2;clock_ms=0;assert(headers(7,&s)==-ETIMEDOUT && clock_ms>=30000);
 calls=0;mode=0;assert(!close_request(7) && calls==3);
 calls=0;mode=2;clock_ms=0;assert(close_request(7)==-EAGAIN && clock_ms>=5000);
 calls=0;mode=0;char value[32];size_t n;assert(!range_header(7,value,sizeof(value),&n) && calls==3 && n==11);
 return 0;
}
''')
