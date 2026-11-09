/* SPDX-License-Identifier: MIT - renders the real shared LVGL onboarding UI. */
#include <lvgl.h>
#include "aster_setup.h"
#include "aster_platform.h"
#include "setup_ui.h"
#include "ui_lvgl.h"
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
uint32_t preview_ms;
void aster_ui_lvgl_assert(void){fputs("LVGL assertion failed\n",stderr);abort();}
static aster_setup_snapshot current;
static uint32_t w,h;
static lv_display_t *display;
static lv_indev_t *indev;
static uint16_t *pixels;
void *aster_psram_alloc(size_t n){return malloc(n);}
void aster_psram_free(void *p){free(p);}
static bool missing(const lv_font_t *f,lv_font_glyph_dsc_t *d,uint32_t a,uint32_t b){(void)f;(void)d;(void)a;(void)b;return false;}
const lv_font_t aster_ui_placeholder_font={.line_height=16,.get_glyph_dsc=missing};
bool aster_setup_poll(aster_setup_snapshot *out){*out=current;return current.state.phase!=ASTER_SETUP_DONE;}
void aster_setup_begin(void){}
void aster_setup_retry(void){}
void aster_splash_off(void){}
void aster_font_sd_reload(void){}
bool aster_setup_format_pending(void){return false;}
void aster_setup_format_ui_ready(void){}
void aster_launcher_hide(void){}
void aster_font_sd_cache_drop(void){}
void aster_ui_lvgl_resync_window(void){}
lv_obj_t *aster_ui_lvgl_screen(void){return lv_screen_active();}
uint32_t aster_ui_lvgl_width(void){return w;}
uint32_t aster_ui_lvgl_height(void){return h;}
uint32_t aster_ui_lvgl_layout_width(void){return w;}
uint32_t aster_ui_lvgl_layout_height(void){return h;}
void aster_ui_lvgl_pointer(uint32_t x,uint32_t y,int down){(void)x;(void)y;(void)down;}
lv_indev_t *aster_ui_lvgl_indev(void){return indev;}
void aster_ui_lvgl_pump(void){lv_tick_inc(200);preview_ms+=200;lv_timer_handler();}
int aster_plat_input_poll(aster_touch_evt_t *e,int n){(void)e;(void)n;return 0;}
static void flush(lv_display_t *d,const lv_area_t *area,uint8_t *buffer){(void)area;(void)buffer;lv_display_flush_ready(d);}
static void read_pointer(lv_indev_t *i,lv_indev_data_t *data){(void)i;data->state=LV_INDEV_STATE_RELEASED;}
static void save(const char *file){
 FILE *f=fopen(file,"wb");assert(f);fprintf(f,"P6\n%u %u\n255\n",w,h);
 for(size_t i=0;i<(size_t)w*h;i++){uint16_t p=pixels[i];uint8_t rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};assert(fwrite(rgb,1,3,f)==3);}fclose(f);
}
static void bounds(lv_obj_t *parent){
 for(unsigned i=0;i<lv_obj_get_child_count(parent);i++){
  lv_obj_t *child=lv_obj_get_child(parent,i);lv_area_t a;lv_obj_get_coords(child,&a);
  if(lv_obj_check_type(child,&lv_bar_class))for(unsigned j=0;j<i;j++) {
   lv_obj_t *prior=lv_obj_get_child(parent,j);lv_area_t p;lv_obj_get_coords(prior,&p);
   if(lv_obj_check_type(prior,&lv_label_class) && p.y2>=a.y1-4) {
    fprintf(stderr,"progress bar overlaps text: %s\n",lv_label_get_text(prior));abort();
   }
  }
  /* Labels must remain on glass; this catches small-screen wrapping/clipping. */
  if(lv_obj_check_type(child,&lv_label_class) && (a.y1<0 || a.y2>=(int)h || a.x1<0 || a.x2>=(int)w)){
   fprintf(stderr,"clipped %s: %d,%d..%d,%d on %ux%u\n",lv_label_get_text(child),a.x1,a.y1,a.x2,a.y2,w,h);abort();
  }
  bounds(child);
 }
}
int main(int argc,char **argv){
 assert(argc==4);w=atoi(argv[1]);h=atoi(argv[2]);lv_init();display=lv_display_create(w,h);
 pixels=calloc((size_t)w*h,2);assert(pixels);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
 lv_display_set_buffers(display,pixels,NULL,w*h*2,LV_DISPLAY_RENDER_MODE_DIRECT);lv_display_set_flush_cb(display,flush);
 indev=lv_indev_create();lv_indev_set_type(indev,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(indev,read_pointer);
 strcpy(current.ssid,"Aster-Setup-1234");strcpy(current.password,"TEST23456789");
 const int phases[]={ASTER_SETUP_WELCOME,ASTER_SETUP_PROVISION,ASTER_SETUP_DOWNLOAD,ASTER_SETUP_ERROR};
 for(unsigned i=0;i<4;i++){
  current.state.phase=phases[i];current.state.bytes=42*1048576;current.state.total=96*1048576;current.state.file=5;current.state.files=11;current.seconds=31;strcpy(current.file,"fonts/ui_20.cpfont");
  current.state.error=phases[i]==ASTER_SETUP_ERROR?-110:0;
  for(unsigned n=0;n<3;n++)aster_setup_ui_tick();lv_obj_update_layout(lv_screen_active());bounds(lv_screen_active());
  char name[512];snprintf(name,sizeof(name),"%s/%ux%u-%d.ppm",argv[3],w,h,phases[i]);save(name);
 }
 current.state.phase=ASTER_SETUP_DONE;assert(!aster_setup_ui_tick());puts("PASS setup UI bounds and teardown");return 0;
}
