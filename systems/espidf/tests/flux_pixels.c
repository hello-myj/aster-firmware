/* SPDX-License-Identifier: MIT */
/* Execute the same fused renderer in both panel byte orders. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl.h>
#include <pthread.h>
#include "draw/lv_draw_private.h"
#include "flux_draw.h"
static uint16_t swap(uint16_t p) {return (p<<8)|(p>>8);}
static void check_flux_pixels888(void);
void check_flux_pixels(void)
{
    const unsigned count=480*480;
    uint16_t *source=malloc(360*140*2),*native=malloc(count*2),*wire=malloc(count*2);
    assert(source && native && wire);
    for(unsigned y=0;y<140;y++)for(unsigned x=0;x<360;x++)
        source[y*360+x]=(x<30 || x>320)?0x07e0:(uint16_t)((x*71+y*113)&65535);
    aster_flux_draw_prepare(source,720);
    for(unsigned trial=0;trial<4;trial++) {
        for(unsigned i=0;i<count;i++){native[i]=0x39e7;wire[i]=swap(native[i]);}
        lv_draw_buf_t a,b;
        assert(lv_draw_buf_init(&a,480,480,LV_COLOR_FORMAT_RGB565,960,native,count*2)==LV_RESULT_OK);
        assert(lv_draw_buf_init(&b,480,480,LV_COLOR_FORMAT_RGB565_SWAPPED,960,wire,count*2)==LV_RESULT_OK);
        lv_area_t clip={13,17,411,268},coords={40,60,399,199};
        lv_layer_t layer={.draw_buf=&a,.color_format=LV_COLOR_FORMAT_RGB565,.buf_area={0,0,479,479}};
        lv_draw_task_t task={.target_layer=&layer,.clip_area=clip};
        lv_draw_image_dsc_t image;lv_draw_image_dsc_init(&image);
        image.scale_x=image.scale_y=trial==0?256:174+trial*19;
        image.rotation=trial==0?0:(trial&1?100:3500);image.pivot=(lv_point_t){180,70};
        image.opa=trial==2?143:255;image.antialias=trial!=3;
        assert(aster_flux_draw_image(1,LV_COLOR_FORMAT_RGB565,source,&coords,720,&clip,&task,&image)==LV_RESULT_OK);
        layer.draw_buf=&b;layer.color_format=LV_COLOR_FORMAT_RGB565_SWAPPED;
        assert(aster_flux_draw_image(1,LV_COLOR_FORMAT_RGB565,source,&coords,720,&clip,&task,&image)==LV_RESULT_OK);
        unsigned changed=0;
        for(unsigned i=0;i<count;i++) {
            assert(native[i]==swap(wire[i]));
            unsigned x=i%480,y=i/480;
            if(x<13 || x>411 || y<17 || y>268)assert(native[i]==0x39e7);
            if(native[i]!=0x39e7)changed++;
        }
        assert(changed>10000);
        if(trial==0)assert(native[100*480+100]==source[40*360+60]);
    }
    aster_flux_draw_forget(source);free(source);free(native);free(wire);
    check_flux_pixels888();
}

typedef struct {aster_flux_rows_fn fn;void *context;int first,last;} host_rows_t;
static unsigned parallel_jobs;
static void *host_row_worker(void *value) {host_rows_t *j=value;j->fn(j->context,j->first,j->last);return NULL;}
static void host_parallel(aster_flux_rows_fn fn,void *context,int first,int middle,int last)
{
    host_rows_t job={fn,context,first,middle-1};pthread_t worker;
    assert(!pthread_create(&worker,NULL,host_row_worker,&job));
    fn(context,middle,last);assert(!pthread_join(worker,NULL));parallel_jobs++;
}
static void check_flux_pixels888(void)
{
    const unsigned bytes=480*480*3;
    uint8_t *source=malloc(360*140*3),*plain=aligned_alloc(64,bytes),*cached=aligned_alloc(64,bytes);assert(source && plain && cached);
    for(unsigned y=0;y<140;y++)for(unsigned x=0;x<360;x++) {
        uint8_t *p=source+(y*360+x)*3;
        if(x<30 || x>320){p[0]=0x17;p[1]=0x29;p[2]=0x3b;}
        else {p[0]=x*7+y*13;p[1]=x*11+y*3;p[2]=x*3+y*17;}
    }
    for(unsigned trial=0;trial<4;trial++) {
        memset(plain,0x35,bytes);memset(cached,0x35,bytes);
        lv_area_t clip={13,17,411,268},coords={40,60,399,199};
        lv_draw_buf_t buf;assert(lv_draw_buf_init(&buf,480,480,LV_COLOR_FORMAT_RGB888,1440,plain,bytes)==LV_RESULT_OK);
        lv_layer_t layer={.draw_buf=&buf,.color_format=LV_COLOR_FORMAT_RGB888,.buf_area={0,0,479,479}};
        lv_draw_task_t task={.target_layer=&layer,.clip_area=clip};lv_draw_image_dsc_t d;lv_draw_image_dsc_init(&d);
        d.scale_x=trial?193:256;d.scale_y=trial==2?170:d.scale_x;d.rotation=trial==0?0:trial&1?100:3500;
        d.pivot=(lv_point_t){180,70};d.opa=trial==1?143:255;d.antialias=trial!=0;
        aster_flux_set_parallel(NULL);aster_flux_draw_forget(source);
        assert(aster_flux_draw_image(1,LV_COLOR_FORMAT_RGB888,source,&coords,1080,&clip,&task,&d)==LV_RESULT_OK);
        aster_flux_draw_prepare_format(source,1080,LV_COLOR_FORMAT_RGB888);buf.data=cached;
        aster_flux_set_parallel(host_parallel);unsigned previous=parallel_jobs;
        assert(aster_flux_draw_image(1,LV_COLOR_FORMAT_RGB888,source,&coords,1080,&clip,&task,&d)==LV_RESULT_OK);
        assert(parallel_jobs>previous);assert(!memcmp(plain,cached,bytes));unsigned changed=0;
        for(unsigned y=0;y<480;y++)for(unsigned x=0;x<480;x++)for(unsigned c=0;c<3;c++) {
            uint8_t v=cached[(y*480+x)*3+c];if(v!=0x35)changed++;
            if(x<13 || x>411 || y<17 || y>268)assert(v==0x35);
        }
        assert(changed>30000);
        if(!trial)assert(!memcmp(cached+(100*480+100)*3,source+(40*360+60)*3,3));
    }
    aster_flux_set_parallel(NULL);aster_flux_draw_forget(source);free(source);free(plain);free(cached);
}
