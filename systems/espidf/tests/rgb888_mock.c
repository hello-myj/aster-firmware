/* SPDX-License-Identifier: MIT */
#include "aster_idf_runtime.h"
#include <lvgl.h>
#include <stdio.h>
static unsigned calls,errors;
static int legacy(const uint8_t *p,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch)
{(void)p;(void)x;(void)y;(void)w;(void)h;(void)pitch;errors++;return -1;}
static int present(const uint8_t *p,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch)
{
    if(calls==0 || calls==2) {if(x || y || w!=64 || h!=64 || p[0]!=3 || p[1]!=5 || p[2]!=7)errors++;}
    if(calls==3) {if(x!=2 || y!=3 || w!=1 || h!=2 || pitch!=1 || p[0]!=0x3b || p[1]!=0x29 || p[2]!=0x17 || p[3]!=0x65 || p[4]!=0x53 || p[5]!=0x41)errors++;}
    calls++;return errors?-1:0;
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    extern void aster_idf_render_init(void);aster_idf_render_init();
    lv_display_t *display=lv_display_create(480,480);if(!display)return 3;
    aster_idf_io_t io={.width=480,.height=480,.present=legacy,.present_bgr888=present};aster_idf_set_io(&io);
    for(unsigned run=0;run<2;run++) {calls=errors=0;int r=aster_idf_run_wasm(argv[1]);if(r || errors || calls!=4){fprintf(stderr,"rgb888 r=%d errors=%u calls=%u\n",r,errors,calls);return 1;}}
    puts("PASS RGB888/BGR888 surfaces, low bits, >50 red levels, negotiated commands, atomic validation and pitched RGB flush");return 0;
}
