"""Exercise the real bootstrap batching loop, including partial network reads."""
import unittest
from test_waveshare import ROOT, run_c


class SetupStream(unittest.TestCase):
    def test_batches_tail_retries_limits_and_failed_transfer(self):
        source = (ROOT / 'systems/shared/setup/aster_setup.c').read_text()
        marker = source.index('/* The transport may publish')
        start = source.rfind('while (!rc)', 0, marker)
        loop = source[start:source.index('        if (opened)', marker)]
        run_c(r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define OWNER 123
struct context {uint8_t buffer[16384];uint64_t completed,total;char relative[32];};
static uint64_t clock_ms, input, available, output;
static unsigned calls,writes,mode;
static int64_t k_uptime_get(void){return clock_ms;}
static void k_msleep(unsigned ms){clock_ms+=ms;}
static int aster_http_svc_read(uint64_t owner,int h,void *out,size_t cap){
 assert(owner==OWNER && h==7 && cap);calls++;
 if(calls%3==0)return -EAGAIN;
 if(mode==1 && input>=4096)return -EIO;
 if(input==available)return 0;
 size_t n=available-input;if(n>2048)n=2048;if(n>cap)n=cap;
 for(size_t i=0;i<n;i++)((uint8_t*)out)[i]=(uint8_t)(input+i);
 input+=n;return (int)n;
}
static int aster_fs_write_safe(int *f,const void *data,size_t n){
 assert(f && n && n<=16384);writes++;
 if(mode==2)return -ENOSPC;
 for(size_t i=0;i<n;i++)assert(((const uint8_t*)data)[i]==(uint8_t)(output+i));
 output+=n;return (int)n;
}
static void progress(uint64_t a,uint64_t b,unsigned i,const char *s){
 assert(a==output && a<=b && i==1 && s);
}
static int transfer(uint64_t bytes){
 struct context storage={.total=bytes},*c=&storage;
 uint64_t offset=0,write_ms=0,wait_ms=0,received=0;
 unsigned reads=0,empty_reads=0,index=1;
 size_t buffered=0;int rc=0,h=7,f=0;
''' + loop + r'''
 assert(offset==output);
 (void)write_ms;(void)wait_ms;(void)received;(void)reads;(void)empty_reads;
 return rc;
}
static void reset(uint64_t n,unsigned m){
 clock_ms=input=output=0;available=n;calls=writes=0;mode=m;
}
int main(void){
 reset(32768+123,0);assert(!transfer(available));assert(output==available && writes==3);
 reset(32768,0);assert(!transfer(available));assert(output==available && writes==2);
 reset(1000,0);assert(!transfer(available));assert(output==1000 && writes==1);
 reset(20000,1);assert(transfer(available)==-EIO && output==0 && writes==0);
 reset(20000,2);assert(transfer(available)==-ENOSPC && output==0 && writes==1);
 reset(2048,0);assert(transfer(1024)==-EFBIG && output==0 && writes==0);
 return 0;
}
''')
