/* SPDX-License-Identifier: MIT */
#include "aster_idf_runtime.h"
#include <stdio.h>
static int calls, reads, mismatch;
static int present(const uint8_t *bytes,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch)
{
    const uint16_t *p=(const uint16_t *)bytes;
    if (calls++==0) {
        if (x || y || w!=480 || h!=480 || pitch!=480) mismatch=1;
        for (unsigned i=0;i<480*480;++i) if (p[i]!=0x07e0) mismatch=1;
#ifdef TEST_RENDER
    } else if (calls==3) {
        if (x || y || w!=480 || h!=480 || pitch!=480 || p[0]!=0x001f || p[10+10*480]!=0xf800) mismatch=1;
#endif
    } else {
        if (x!=1 || y!=2 || w!=2 || h!=2 || pitch!=3 ||
            p[0]!=0xf800 || p[1]!=0x001f || p[3]!=0x1234 || p[4]!=0x5678) mismatch=1;
    }
    return mismatch ? -1 : 0;
}
static int touch(uint32_t *x,uint32_t *y)
{
    switch (reads++) {
    case 0: *x=10; *y=20; return 1;
    case 1: return -1; /* I/O failure must not synthesize UP. */
    case 2: *x=30; *y=40; return 1;
    default: return 0;
    }
}
#ifdef TEST_RENDER
#define EXPECTED_PRESENTS 3
#else
#define EXPECTED_PRESENTS 2
#endif
int main(int argc,char **argv)
{
    if (argc!=2) return 2;
    const aster_idf_io_t io={.width=480,.height=480,.present=present,.touch=touch};
    aster_idf_set_io(&io);
    for (int run=0;run<2;++run) {
        calls=reads=mismatch=0;
        int result=aster_idf_run_wasm(argv[1]);
        if (result || mismatch || calls!=EXPECTED_PRESENTS || reads!=4) {
            fprintf(stderr,"result=%d mismatch=%d presents=%d reads=%d\n",result,mismatch,calls,reads);
            return 1;
        }
    }
    puts("PASS graphics, pitched flush, stale handles, touch errors/edges and repeated lifecycle");
    return 0;
}
