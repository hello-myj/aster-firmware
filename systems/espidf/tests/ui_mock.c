/* SPDX-License-Identifier: MIT. Hardware callbacks here are test doubles. */
#include "aster_idf_ui.h"
#include "aster_idf_runtime.h"
#include "aster_platform.h"
#include "aster_storage.h"
#include "aster_app_mgr.h"
#include "font_sd.h"
#include "ui_lvgl.h"
#include "launcher.h"
#include "qpanel.h"
#include "wifi_service.h"
#include <aster/time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
static uint16_t pixels[480*480],snapshot[480*480];
#ifdef CONFIG_ASTER_P4_RGB666_OUTPUT
static uint8_t native_pixels[480*480*3],native_snapshot[480*480*3];
static const uint8_t *capture_native(void){return native_pixels;}
static int present_native(const uint8_t *p,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t pitch)
{
    for(unsigned row=0;row<h;row++)for(unsigned col=0;col<w;col++) {
        const uint8_t *a=p+((size_t)row*pitch+col)*3;uint8_t *b=native_pixels+((y+row)*480+x+col)*3;
        memcpy(b,a,3);pixels[(y+row)*480+x+col]=((uint16_t)(a[2]>>3)<<11)|((uint16_t)(a[1]>>2)<<5)|(a[0]>>3);
    }
    return 0;
}
#endif
static uint32_t x,y;static bool contact;static int volume=70;
static int present(const uint8_t *source,uint32_t left,uint32_t top,uint32_t width,uint32_t height,uint32_t pitch)
{
    if(left+width>480 || top+height>480 || pitch<width)return -1;
    for(unsigned row=0;row<height;row++)memcpy(pixels+(top+row)*480+left,source+row*pitch*2,width*2);
#ifdef CONFIG_ASTER_P4_RGB666_OUTPUT
    for(unsigned row=0;row<height;row++)for(unsigned col=0;col<width;col++) {
        uint16_t c=pixels[(top+row)*480+left+col];uint8_t *b=native_pixels+((top+row)*480+left+col)*3;
        b[0]=(c&31)*255/31;b[1]=((c>>5)&63)*255/63;b[2]=(c>>11)*255/31;
    }
#endif
    return 0;
}
static int touch(uint32_t *out_x,uint32_t *out_y) {*out_x=x;*out_y=y;return contact;}
static const uint8_t *capture(void) {return (uint8_t *)pixels;}
static int launch(const char *id) {(void)id;return ASTER_ESTATE;}
static void pause_ms(unsigned ms) {struct timespec delay={ms/1000,(ms%1000)*1000000};nanosleep(&delay,NULL);}
static void pump(unsigned ms) {for(unsigned n=0;n<ms;n+=5){aster_idf_ui_idle();pause_ms(5);}}
static int save(const char *directory,const char *name)
{
    char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);
    FILE *file=fopen(path,"wb");if(!file)return 1;fprintf(file,"P6\n480 480\n255\n");
    for(unsigned i=0;i<480*480;i++) {
        uint16_t p=pixels[i];uint8_t rgb[]={(uint8_t)(((p>>11)&31)*255/31),(uint8_t)(((p>>5)&63)*255/63),(uint8_t)((p&31)*255/31)};
#ifdef CONFIG_ASTER_P4_RGB666_OUTPUT
        rgb[0]=native_pixels[i*3+2];rgb[1]=native_pixels[i*3+1];rgb[2]=native_pixels[i*3];
#endif
        fwrite(rgb,1,3,file);
    }
    return fclose(file)!=0;
}
/* Radio and codec are absent in this host test. Only native UI logic runs. */
int aster_idf_wifi_snapshot(aster_wifi_info_t *out)
{*out=(aster_wifi_info_t){.size=sizeof(*out),.version=1,.state=ASTER_WIFI_STATE_DISABLED};return 0;}
int aster_idf_wifi_results(aster_wifi_ap_info_t *out,uint32_t max) {(void)out;(void)max;return ASTER_ENOENT;}
int aster_idf_wifi_saved(char out[33]) {out[0]=0;return ASTER_ENOENT;}
int aster_idf_wifi_submit(uint32_t op,const char *ssid,const char *password) {(void)op;(void)ssid;(void)password;return ASTER_ESTATE;}
int aster_idf_wifi_hotspot_submit(bool enable,const char *ssid,const char *password) {(void)enable;(void)ssid;(void)password;return ASTER_ESTATE;}
void aster_idf_wifi_hotspot_snapshot(char ssid[33],bool *enabled,bool *configured) {ssid[0]=0;*enabled=*configured=false;}
int aster_plat_audio_get_volume(void) {return volume;}
int aster_plat_audio_set_volume(int level) {volume=level;return 0;}
int aster_plat_audio_ui_play(const int16_t *pcm,size_t frames,unsigned gain,unsigned priority)
{(void)pcm;(void)frames;(void)gain;(void)priority;return -ENODEV;}
void aster_plat_audio_ui_stop(void) {}
void aster_plat_audio_ui_release(void) {}
int aster_time_read(aster_time_info_t *out)
{*out=(aster_time_info_t){.size=sizeof(*out),.version=1,.year=2026,.month=10,.day=31,.hour=12,.minute=34};return 0;}
int main(int argc,char **argv)
{
    if(argc!=3 || aster_storage_init() || aster_app_mgr_init()<0)return 2;
    for(unsigned i=0;i<9;i++) {
        char path[64];snprintf(path,sizeof(path),"/SD:/test.ui.%u.ast",i);
        if(aster_app_mgr_install(path))return 10;
    }
    const aster_idf_io_t io={.width=480,.height=480,.present=present,.touch=touch,
#ifdef CONFIG_ASTER_P4_RGB666_OUTPUT
        .present_bgr888=present_native,.capture_bgr888=capture_native,
#endif
    };aster_idf_set_io(&io);aster_idf_set_capture(capture);
    if(aster_idf_ui_init(launch))return 3;
    extern void check_flux_pixels(void);check_flux_pixels();
    /* Give the actual cpfont reader and FLUX splash/catalog warmup time. */
    for(unsigned n=0;n<14000;n+=50) {
        pump(50);
        if(n>1000 && aster_font_sd_get(ASTER_FONT_UI_PX) && !aster_font_sd_busy()) {pump(300);break;}
    }
    if(save(argv[1],"desktop"))return 4;
    /* Cross more entries than the visible five, then revisit them. Exercise
     * real bake/retire/rebind paths, and require cached pixels to be reused. */
    for(unsigned i=0;i<20;i++){aster_launcher_step_page(1);pump(350);}
    uint32_t bakes,hits,cached;aster_launcher_art_stats(&bakes,&hits,&cached);
    if(!hits || !cached)return 11;
    memcpy(snapshot,pixels,sizeof(pixels));
    x=20;y=5;contact=true;pump(50);y=80;pump(50);contact=false;pump(100);
    /* FLUX maps the home pull-down to its full mixer page, not the APP sheet. */
    if(!memcmp(snapshot,pixels,sizeof(pixels))) {fputs("host desktop pull-down failed\n",stderr);return 5;}
    if(save(argv[1],"desktop-panel"))return 6;
    aster_qpanel_force_close();aster_idf_ui_before_app();
    aster_launcher_art_stats(&bakes,&hits,&cached);if(cached)return 12;
    for(unsigned i=0;i<480*480;i++)pixels[i]=0x001f;
#ifdef CONFIG_ASTER_P4_RGB666_OUTPUT
    /* Low bits must survive opening/closing the panel, even if not 565-representable. */
    for(unsigned i=0;i<480*480;i++){native_pixels[i*3]=0x17;native_pixels[i*3+1]=0x29;native_pixels[i*3+2]=0x3b;}
    memcpy(native_snapshot,native_pixels,sizeof(native_pixels));
#endif
    aster_qpanel_open(ASTER_QPANEL_OVER_APP);
    for(unsigned i=0;i<20;i++){aster_qpanel_tick();aster_ui_lvgl_pump();pause_ms(5);}
    if(!aster_idf_app_glass_frozen() || save(argv[1],"app-panel"))return 7;
    memcpy(snapshot,pixels,sizeof(pixels));
    if(aster_idf_run_wasm(argv[2]) || memcmp(snapshot,pixels,sizeof(pixels)))return 8;
    aster_qpanel_close(true);
#ifdef CONFIG_ASTER_P4_RGB666_OUTPUT
    if(memcmp(native_pixels,native_snapshot,sizeof(native_pixels)))return 9;
#else
    for(unsigned i=0;i<480*480;i++)if(pixels[i]!=0x001f)return 9;
#endif
    aster_idf_ui_after_app();
    puts("PASS native FLUX render, pull-down gesture, APP overlay freeze/restore and panel-canvas hint fallback");
    return 0;
}
