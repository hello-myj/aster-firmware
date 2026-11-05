/* SPDX-License-Identifier: MIT. Cooperative install UI on the existing worker. */
#include "install_progress.h"
#include "ui_lvgl.h"
#include "ui_font.h"
#include "aster_platform.h"
#include "touch_ui.h"
#include "gesture.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
static lv_obj_t *root,*stage,*percent,*bar,*size_label,*clock_label,*file_label,*detail_label;
static uint32_t since,last_pump,last_draw;
static int previous=-1;
static bool expanded;
static struct aster_touch_ui_state touch_state;
static lv_obj_t *label(int y,int px)
{
 lv_obj_t *o=lv_label_create(root);lv_obj_set_pos(o,18,y);
 lv_obj_set_width(o,aster_ui_lvgl_layout_width()-36);
 lv_obj_set_style_text_font(o,aster_ui_font(px),0);
 lv_obj_set_style_text_color(o,lv_color_hex(0x26364c),0);return o;
}
static void details(lv_event_t *e)
{
 (void)e;expanded=!expanded;
 if(expanded)lv_obj_remove_flag(file_label,LV_OBJ_FLAG_HIDDEN);
 else lv_obj_add_flag(file_label,LV_OBJ_FLAG_HIDDEN);
 lv_label_set_text(detail_label,expanded?"收起详情":"查看详情");
}
static void create(void)
{
 aster_ui_lvgl_resync_window();lv_obj_t *screen=aster_ui_lvgl_screen();if(!screen)return;
 int w=aster_ui_lvgl_layout_width(),h=aster_ui_lvgl_layout_height(),offset=(h>320?(h-320)/2:0);
 root=lv_obj_create(screen);lv_obj_remove_style_all(root);lv_obj_set_size(root,w,h);
 lv_obj_set_style_bg_color(root,lv_color_hex(0xf2f5fa),0);lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);
 lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);lv_obj_move_foreground(root);
 lv_label_set_text(label(offset+24,20),"正在安装应用");
 stage=label(offset+68,16);percent=label(offset+104,24);
 bar=lv_bar_create(root);lv_obj_set_pos(bar,18,offset+146);lv_obj_set_size(bar,w-36,12);lv_bar_set_range(bar,0,1000);
 lv_obj_set_style_bg_color(bar,lv_color_hex(0xdce4f0),LV_PART_MAIN);
 lv_obj_set_style_bg_color(bar,lv_color_hex(0x2466dc),LV_PART_INDICATOR);
 size_label=label(offset+172,12);clock_label=label(offset+194,12);
 lv_obj_t *button=lv_button_create(root);lv_obj_set_pos(button,18,offset+222);lv_obj_set_size(button,w-36,34);
 lv_obj_add_event_cb(button,details,LV_EVENT_CLICKED,NULL);
 detail_label=lv_label_create(button);lv_obj_set_style_text_font(detail_label,aster_ui_font(14),0);lv_label_set_text(detail_label,"查看详情");lv_obj_center(detail_label);
 file_label=label(offset+268,10);lv_label_set_long_mode(file_label,LV_LABEL_LONG_MODE_WRAP);lv_obj_set_height(file_label,40);lv_obj_add_flag(file_label,LV_OBJ_FLAG_HIDDEN);
 aster_ui_font_warm(20,"正在安装应用");aster_ui_font_warm(16,"正在检查安装包正在复制资源正在同步文件正在提交版本安装完成");
 aster_ui_font_warm(14,"查看详情收起详情");aster_ui_font_warm(12,"已复制用时秒准备中");
 since=lv_tick_get();last_pump=last_draw=since-1000;previous=-1;expanded=false;aster_touch_ui_reset(&touch_state);
}
void aster_ui_install_progress(void *context,aster_install_phase phase,uint64_t done,uint64_t total,const char *file)
{
 (void)context;if(!root)create();if(!root)return;
 static uint32_t diagnostic_at;
 if(lv_tick_get()-diagnostic_at>=5000||(int)phase!=previous){size_t unused=0;(void)k_thread_stack_space_get(k_current_get(),&unused);printk("INSTALL phase=%u copied=%llu/%llu stack_free=%u\n",(unsigned)phase,(unsigned long long)done,(unsigned long long)total,(unsigned)unused);diagnostic_at=lv_tick_get();}
 uint32_t now=lv_tick_get();if(now-last_pump<50&&(int)phase==previous)return;last_pump=now;
 lv_indev_t *pointer=aster_ui_lvgl_indev();aster_touch_evt_t events[16];
 for(unsigned batch=0;batch<4;batch++){
  int n=aster_plat_input_poll(events,16);if(n<=0)break;
  uint32_t masks[16];for(int i=0;i<n;i++)masks[i]=events[i].contact_mask;
  aster_touch_ui_begin_batch(&touch_state,masks,(uint8_t)n);
  for(int i=0;i<n;i++){
   struct aster_touch_ui_decision d=aster_touch_ui_accept(&touch_state,(uint8_t)i,events[i].action,events[i].slot,events[i].contact_mask);
   if(d.release_pointer){lv_indev_wait_release(pointer);aster_ui_lvgl_pointer(0,0,0);}
   if(d.route)aster_ui_lvgl_pointer(events[i].x,events[i].y,events[i].action!=ASTER_TOUCH_UP);
   if(pointer)lv_indev_read(pointer);
  }
 }
 if(now-last_draw>=200||(int)phase!=previous){
  static const char *names[]={"正在检查安装包","正在复制资源","正在同步文件","正在提交版本","安装完成"};
  unsigned index=(unsigned)phase;if(index>ASTER_INSTALL_DONE)index=ASTER_INSTALL_CHECK;
  lv_label_set_text(stage,names[index]);char text[160];
  unsigned value=total?(unsigned)((done>total?total:done)*990/total):0;if(phase==ASTER_INSTALL_DONE)value=1000;
  if(total||phase==ASTER_INSTALL_DONE)snprintf(text,sizeof(text),"%u%%",value/10);else snprintf(text,sizeof(text),"准备中…");
  lv_label_set_text(percent,text);lv_bar_set_value(bar,value,LV_ANIM_OFF);
  snprintf(text,sizeof(text),"已复制 %llu / %llu KiB",(unsigned long long)(done/1024),(unsigned long long)(total/1024));lv_label_set_text(size_label,text);
  snprintf(text,sizeof(text),"用时 %u 秒",(unsigned)((now-since)/1000));lv_label_set_text(clock_label,text);
  const char *name=file?strrchr(file,'/'):NULL;snprintf(text,sizeof(text),"%.120s",name?name+1:file?file:"");lv_label_set_text(file_label,text);
  previous=(int)phase;last_draw=now;
 }
 if(aster_ui_fonts_poll())lv_obj_invalidate(root);
 aster_ui_lvgl_request_frame();aster_ui_lvgl_pump();
}
void aster_ui_install_end(void)
{
 if(!root)return;lv_obj_delete(root);root=NULL;
 aster_ui_lvgl_pointer(0,0,0);lv_indev_wait_release(aster_ui_lvgl_indev());aster_gesture_reset();aster_touch_ui_reset(&touch_state);
}
