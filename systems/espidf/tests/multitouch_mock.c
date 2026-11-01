/* SPDX-License-Identifier: MIT */
#include "aster_idf_runtime.h"
#include <stdio.h>
static unsigned reads;
static int present(const uint8_t *p,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch)
{(void)p;(void)x;(void)y;(void)w;(void)h;(void)pitch;return 0;}
static int frame(aster_idf_touch_point_t p[ASTER_IDF_MAX_TOUCH])
{
    unsigned step=reads++;
    for(unsigned i=0;i<5;i++){p[i].x=10+i*50;p[i].y=20+i*50;}
    switch(step) {
    case 0:return 31;
    case 1:return -1; /* bus failure: preserve every contact */
    case 2:p[2].x=480;return 31; /* reject entire malformed frame */
    case 3:return 31; /* stationary: no MOVE noise */
    case 4:for(unsigned i=0;i<5;i++)p[i].x++;return 31;
    case 5:for(unsigned i=0;i<5;i++)p[i].x++;return 21; /* holes 1 and 3 */
    case 6:for(unsigned i=0;i<5;i++)p[i].x+=2;return 21;
    case 7:return 0;
    case 8:return 8; /* re-use hardware slot 3, never compact it to 0 */
    default:return 0;
    }
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    aster_idf_io_t io={.width=480,.height=480,.present=present,.touch_frame=frame,.max_touch_points=5};
    aster_idf_set_io(&io);
    for(unsigned run=0;run<2;run++) {
        reads=0;int result=aster_idf_run_wasm(argv[1]);
        if(result || reads!=10){fprintf(stderr,"multitouch result=%d reads=%u\n",result,reads);return 1;}
    }
    puts("PASS five contacts, stable sparse slots, frame masks, snapshots, I/O errors, bounds and lifecycle");return 0;
}
