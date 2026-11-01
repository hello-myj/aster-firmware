/* SPDX-License-Identifier: MIT */
#include "aster_idf_runtime.h"
#include <stdio.h>
static unsigned calls,reads;static bool wrong;
static int present(const uint8_t *bytes,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch)
{
    calls++;if(x || y || w!=480 || h!=480 || pitch!=480)return -1;
    const uint16_t *p=(const uint16_t *)bytes;
    for(unsigned row=0;row<480;row++)for(unsigned col=0;col<480;col++) {
        uint16_t expected=row<120 || row>=360?0:row<240?(col<240?0xf800:0x001f):(col<240?0x07e0:0xffff);
        if(p[row*480+col]!=expected)wrong=true;
    }
    return wrong?-1:0;
}
static int touch(uint32_t *x,uint32_t *y) {*x=*y=240;return reads++==0;}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    const aster_idf_io_t io={.width=480,.height=480,.present=present,.touch=touch};aster_idf_set_io(&io);
    int r=aster_idf_run_wasm(argv[1]);
    if(r || wrong || calls!=1 || reads!=2)return 1;
    puts("PASS scale-fit pixels, black margins and inverse touch/poll mapping");return 0;
}
