/* SPDX-License-Identifier: MIT
 * FLUX Rev.04, native LVGL port of the supplied 320x480 design.
 * Widget, registry and filesystem mutations run only on the UI worker.
 * Icons keep their original RGB565/A8 planes; only five ribbons are resident.
 */
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <lvgl.h>
#include <aster/icon.h>
#include <aster/event.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "launcher.h"
#include "ui_lvgl.h"
#include "ui_font.h"
#include "flux_font.h"
#include "flux_draw.h"
#include "font_sd.h"
#include "ui_splash.h"
#include "wifi_settings.h"
#include "qpanel.h"
#include "gesture.h"
#include "aster_platform.h"
#include "aster_app_mgr.h"
#include "aster_runtime.h"
#include "aster_storage.h"
#include "aster_mem.h"
#include "aster_kv.h"
#include <aster/font_metrics.h>
#include "aster_time.h"
LOG_MODULE_REGISTER(aster_flux, LOG_LEVEL_INF);
#include "flux_icons.inc"
#include "flux_motion.h"
#include "flux_sounds.inc"

#define BG 0x131317
#define FG 0xf3f3e8
#define ACCENT 0xd8ff8c
#define DIM 0xa0ae92
#define INK 0x211d28
#define MAX_ENTRIES (ASTER_APP_MAX_RECS + 4)
#define RIBBONS 5
#define FLUX_FRAME_MS 16
#define KV_ID "aster.launcher.flux"
enum { HOME, FILTERS, MIXER, FILES, CLOCK, SYSTEM };
enum { ACT_NONE, ACT_BACK, ACT_FILTERS, ACT_MIXER, ACT_PREV, ACT_NEXT,
       ACT_OPEN, ACT_WIFI, ACT_QUIET, ACT_MOTION, ACT_SYSTEM,
       ACT_CANCEL, ACT_UNINSTALL, ACT_GROUP=32 };
enum { ICON_SETTINGS, ICON_LEFT, ICON_RIGHT, ICON_CLOCK, ICON_FILES,
       ICON_TERMINAL, ICON_BT, ICON_MOON, ICON_WIFI, ICON_MOVE, ICON_BATTERY, ICON_BOX, ICON_ARROW, ICON_ASTER };

typedef struct {
 char id[64], name[40];
 const char *code;
 char description[ASTER_APP_DESCRIPTION_MAX];
 uint32_t color;
 uint8_t group, builtin, icon;
} entry_t;
typedef struct {
 const char *id, *code;
 uint32_t color;
 uint8_t group;
} known_t;
static const known_t known[] = {
 {"aster.player","MUSIC",0xb6a4ff,1},
 {"aster.reader","READING",0x8debc1,1},
 {"aster.gallery","GALLERY",0xf3a8ef,1},
 {"aster.starcatch","PLAY",0xffce8e,2},
 {"aster.game.2048","PLAY",0xff9e6d,2},
 {"aster.snake","PLAY",0xc6e986,2},
 {"aster.imucube","SENSOR",0x76dbde,3},
 {"aster.chess","PLAY",0xffb3b7,2},
 {"aster.neon-duel","PLAY",0xbc99ff,2},
 {"aster.gfxshow","GRAPHICS",0xefafd4,3},
 {"aster.apitest","DEVICE",0x9ddde9,3},
};
static const char *const group_names[]={"全部","媒体","玩乐","探索"};
static const char *const group_codes[]={"FULL SPECTRUM","SOUND & LIGHT","PLAY FREQUENCY","NEW POSSIBILITIES"};
static const uint32_t group_colors[]={ACCENT,0xaaadff,0xffa891,0x89eac6};
/* Launcher state, not a DMA buffer. PSRAM so it does not sit in internal DRAM
 * for the whole time the desktop is up. */
static entry_t entries[MAX_ENTRIES] __attribute__((section(".ext_ram.bss")));
static int entry_n, ids[MAX_ENTRIES], count, group, selected[4];
/* phase is in thousandths of an index, preserving sub-pixel drag motion. */
static int phase, from_phase, target_phase, pull, from_pull;
static int catalog_warmed;
/* Design is 320x480. Each axis scales onto the glass, so a 240x320 panel
 * fills the screen instead of letterboxing a shrunken bitmap. Glyphs stay
 * square; only the spacing follows the two edges. */
static int view_w = 320, view_h = 480;
/* Ribbon art is baked in the 360x140 design. The fused rotator only accepts
 * that size; a scaled bake falls through to an LVGL alloc that halts. */
static int design_px;
static int ax(int v)
{
 if (design_px || !view_w) return v;
 return (int)((int64_t)v * view_w / 320);
}
static int ay(int v)
{
 if (design_px || !view_h) return v;
 return (int)((int64_t)v * view_h / 480);
}
static int as(int v)
{
 int64_t sx, sy, m;
 int r;
 if (design_px || !view_w || !view_h) return v;
 sx = (int64_t)view_w * 480; sy = (int64_t)view_h * 320;
 m = sx < sy ? sx : sy;
 r = (int)((int64_t)v * m / (320 * 480));
 return v && !r ? 1 : r;
}
/* Uniform 256=1x fit. Both ribbon axes share it so the tilt stays square
 * and the fused drawer's tile skip remains valid. */
static int fit256(void)
{
 int sx, sy, f;
 if (!view_w || !view_h) return 256;
 sx = view_w * 256 / 320;
 sy = view_h * 256 / 480;
 f = sx < sy ? sx : sy;
 return f < 1 ? 1 : f;
}
static void to_design(uint32_t *x, uint32_t *y)
{
 if (!view_w || !view_h || !x || !y) return;
 *x = (uint32_t)((uint64_t)*x * 320 / (uint32_t)view_w);
 *y = (uint32_t)((uint64_t)*y * 480 / (uint32_t)view_h);
 if (*x > 319) *x = 319;
 if (*y > 479) *y = 479;
}
static unsigned scroll_duration=FLUX_SNAP_MS;
static bool coasting, draw_pending;
static uint64_t animation_ms, last_draw, hold_ms, clock_sec=UINT64_MAX;
static bool animating, launching, visible, initialized, want_show=true, motion=true, quiet;
static volatile bool dirty;
static volatile int want_page=-1;
static int page, prior_page, action, brightness=90, saved_volume=60;
static int brightness_pending=-1, volume_pending=-1;
static bool volume_save;
static uint64_t save_due;
static char remove_id[64], remove_name[40];
static lv_obj_t *root,*body,*stage,*bar_time,*description,*position,*pull_label,*handle;
static lv_obj_t *filter_label,*modal,*toast,*clock_value,*clock_date,*wifi_state;
static uint64_t toast_until;
static lv_image_dsc_t chrome[ARRAY_SIZE(flux_icon_sizes)][ARRAY_SIZE(flux_icon_data[0])]
	__attribute__((section(".ext_ram.bss")));
static struct {
 lv_obj_t *obj;
 lv_image_dsc_t icon;
 lv_draw_buf_t *art;
 uint8_t *data;
 int entry;
 bool dirty_art;
} ribbons[RIBBONS];
static struct {
 bool down, handle, moved, blocked, stopped;
 int x,y,axis,base,hit;
 uint64_t started;
 struct flux_velocity track;
} drag;
static void show_page(int next);
static void redraw(void);
static void settle(int target);

enum { S_TICK, S_TAP, S_OPEN, S_CLOSE, S_TUNE };
static int sound_entry=-1, sound_group=-1, sound_phase;
static uint64_t sound_tick_ms, sound_sample_ms;
static bool sound_return, sound_launch_started;
static int sound_pending=-1;
static void sound_play(int which, int speed)
{
 if(quiet || volume_pending==0 || !aster_plat_audio_get_volume()) {
  aster_plat_audio_ui_stop();return;
 }
 const int16_t *pcm;size_t frames;unsigned gain=1000;
 uint64_t now=k_uptime_get();
 if(which==S_TICK) {
  unsigned velocity=abs(speed);
  if(sound_tick_ms && now-sound_tick_ms<(velocity>650?160:95)) return;
  gain=MAX(350,1000-(int)velocity*4/10);
 }
 switch(which) {
#define SOUND_CASE(id,name) case id:pcm=sound_##name;frames=ARRAY_SIZE(sound_##name);break
 SOUND_CASE(S_TICK,tick);SOUND_CASE(S_TAP,tap);SOUND_CASE(S_OPEN,open);
 SOUND_CASE(S_CLOSE,close);SOUND_CASE(S_TUNE,tune);
#undef SOUND_CASE
 default:return;
 }
 int err=aster_plat_audio_ui_play(pcm,frames,gain,which==S_TICK?0:1);
 if(!err && which==S_TICK) sound_tick_ms=now;
}

static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color,int radius)
{
 lv_obj_t *o=lv_obj_create(parent);
 lv_obj_remove_style_all(o);
 lv_obj_set_pos(o,ax(x),ay(y));lv_obj_set_size(o,ax(w),ay(h));
 lv_obj_set_style_bg_color(o,lv_color_hex(color),0);
 lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
 lv_obj_set_style_radius(o,as(radius),0);
 lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
 return o;
}
static lv_obj_t *text(lv_obj_t *parent,const char *value,int x,int y,int w,uint32_t color,int size,int zoom)
{
 lv_obj_t *o=lv_label_create(parent);
 unsigned px=aster_flux_font_pixels(size,zoom,value);
 const lv_font_t *native=aster_flux_font((unsigned)as((int)px),value);
 lv_obj_set_pos(o,ax(x),ay(y));lv_obj_set_width(o,ax(w));
 lv_obj_set_style_text_font(o,native?native:aster_ui_font(size),0);
 lv_obj_set_user_data(o,(void *)(uintptr_t)((size<<24)|(zoom<<12)|w));
 lv_obj_set_style_text_color(o,lv_color_hex(color),0);
 lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);lv_label_set_text(o,value);
 lv_obj_clear_flag(o,LV_OBJ_FLAG_CLICKABLE);
 if(!native) aster_ui_font_warm(size,value);
 return o;
}
static void set_text(lv_obj_t *o,const char *value)
{
 if(o && strcmp(lv_label_get_text(o),value)) {
  uintptr_t spec=(uintptr_t)lv_obj_get_user_data(o);int size=spec>>24,zoom=(spec>>12)&4095,width=spec&4095;
  unsigned px=aster_flux_font_pixels(size,zoom,value);
  const lv_font_t *native=aster_flux_font((unsigned)as((int)px),value);
  lv_obj_set_width(o,ax(width));
  lv_obj_set_style_transform_scale_x(o,256,0);
  lv_obj_set_style_transform_scale_y(o,256,0);
  lv_obj_set_style_text_font(o,native?native:aster_ui_font(size),0);
  if(!native) aster_ui_font_warm(size,value);
  lv_label_set_text(o,value);
 }
}
static lv_obj_t *icon(lv_obj_t *parent,int number,int x,int y,int size,uint32_t color)
{
 lv_obj_t *o=lv_image_create(parent);
 unsigned face=0;int want=as(size),bestd=10000;
 for(unsigned i=0;i<ARRAY_SIZE(flux_icon_sizes);i++) {
  int d=flux_icon_sizes[i]-(unsigned)want;if(d<0) d=-d;
  if(d<bestd) {bestd=d;face=i;}
 }
 lv_image_set_src(o,&chrome[face][number]);lv_obj_set_pos(o,ax(x),ay(y));
 lv_image_set_scale(o,256);lv_image_set_pivot(o,0,0);
 lv_obj_set_style_image_recolor(o,lv_color_hex(color),0);
 lv_obj_set_style_image_recolor_opa(o,LV_OPA_COVER,0);
 return o;
}
static void clicked(lv_event_t *e)
{
 action=(int)(intptr_t)lv_event_get_user_data(e);
 LOG_INF("flux: action %d",action);
}
static lv_obj_t *button(lv_obj_t *parent,const char *label,int x,int y,int w,int h,uint32_t color,int act)
{
 lv_obj_t *o=box(parent,x,y,w,h,color,12);
 lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_set_style_bg_opa(o,LV_OPA_80,LV_STATE_PRESSED);
 lv_obj_add_event_cb(o,clicked,LV_EVENT_CLICKED,(void *)(intptr_t)act);
 if(label) {
  lv_obj_t *l=text(o,label,0,0,w,FG,16,256);
  lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(l);
 }
 return o;
}
static void notify(const char *value)
{
 if(toast) lv_obj_delete(toast);
 toast=box(root,22,343,276,48,ACCENT,9);
 text(toast,value,12,13,252,0x354324,16,220);
 toast_until=k_uptime_get()+2200;
}
static int nearest(int p) {return (p+(p<0?-500:500))/1000;}
static int wrap(int i,int n) { return n?((i%n)+n)%n:0; }
static int distance(int index,int p,int n)
{
 if(n<=0) return 0;
 int span=n*1000,d=index*1000-p;
 while(d < -span/2) d+=span;
 while(d >= (span+1)/2) d-=span;
 return d;
}
static entry_t *current(void) { return count?&entries[ids[wrap(selected[group],count)]]:NULL; }
static void save_now(void)
{
 uint8_t state[]={group,selected[0],selected[1],selected[2],selected[3],brightness,motion,quiet};
 (void)aster_kv_set(KV_ID,"state",state,sizeof(state));
 save_due=0;
}
/* Coalesce preference writes until the gesture and settling finish. */
static void save(void) {save_due=k_uptime_get()+750;}
static void read_entries(void)
{
 entry_n=0;
 /* Known IDs retain theme ordering/colors; all application names and
  * introductions belong to package metadata, including known IDs. */
 for(size_t k=0;k<ARRAY_SIZE(known);k++) {
  const aster_app_rec_t *r=aster_app_mgr_find(known[k].id);
  if(!r) continue;
  entry_t *e=&entries[entry_n++];memset(e,0,sizeof(*e));
  snprintf(e->id,sizeof(e->id),"%s",r->id);snprintf(e->name,sizeof(e->name),"%s",r->name);
  e->code=known[k].code;snprintf(e->description,sizeof(e->description),"%s",r->description);e->color=known[k].color;e->group=known[k].group;e->icon=ICON_BOX;
 }
 for(int i=0;i<aster_app_mgr_count() && entry_n<MAX_ENTRIES-4;i++) {
  const aster_app_rec_t *r=aster_app_mgr_get(i);if(!r) continue;
  bool present=false;for(int j=0;j<entry_n;j++) if(!strcmp(entries[j].id,r->id)) present=true;
  if(present) continue;
  entry_t *e=&entries[entry_n++];memset(e,0,sizeof(*e));
  snprintf(e->id,sizeof(e->id),"%s",r->id);snprintf(e->name,sizeof(e->name),"%s",r->name);
  e->group=!strcmp(r->category,"game")?2:3;e->code=e->group==2?"PLAY":"LOCAL APP";
  snprintf(e->description,sizeof(e->description),"%s",r->description);e->color=0xd8c0ff;e->icon=ICON_BOX;
 }
 const entry_t builtin[]={
  {.id="files",.name="文件",.code="STORAGE",.description="每一份收藏，都有它的位置。",.color=0x8fbfff,.group=3,.builtin=FILES,.icon=ICON_FILES},
  {.id="settings",.name="设置",.code="DEVICE MIXER",.description="调到你喜欢的频率。",.color=0xd8c0ff,.group=3,.builtin=MIXER,.icon=ICON_SETTINGS},
  {.id="terminal",.name="系统日志",.code="SYSTEM",.description="系统的声音，从这里传来。",.color=0xb6e8ca,.group=3,.builtin=SYSTEM,.icon=ICON_TERMINAL},
  {.id="clock",.name="时钟",.code="CLOCK",.description="留一点时间，给此刻。",.color=0xb0cbff,.group=3,.builtin=CLOCK,.icon=ICON_CLOCK},
 };
 for(size_t i=0;i<ARRAY_SIZE(builtin);i++) entries[entry_n++]=builtin[i];
 /* Application names come from package metadata and use the generic
  * SD face. Fixed system entries use existing UI fonts. Prewarm titles;
  * introductions warm when selected to avoid evicting the visible text. */
 for(int i=0;i<entry_n;i++)
  if(!entries[i].builtin) aster_ui_font_warm(ASTER_FONT_UI_PX,entries[i].name);

}
static void filter_entries(void)
{
 count=0;for(int i=0;i<entry_n;i++) if(!group || entries[i].group==group) ids[count++]=i;
 selected[group]=wrap(selected[group],count);phase=selected[group]*1000;
 animating=false;pull=0;
}
/* Prewarm the same five-entry window used by the ribbon cache. Dynamic
 * captions and pull labels stay on the generic SD face; fetch them before
 * interaction rather than on the first selection crossing. */
static void warm_visible_text(void)
{
 if(!count) return;
 for(int rel=-2;rel<=2;rel++) {
  entry_t *e=&entries[ids[wrap(nearest(phase)+rel,count)]];
  if(e->builtin) continue;
  char buf[64];
  aster_ui_font_warm(ASTER_FONT_UI_PX,e->name);
  if(e->description[0]) aster_ui_font_warm(ASTER_FONT_CAPTION_PX,e->description);
  snprintf(buf,sizeof(buf),"拉开%s",e->name);
  aster_ui_font_warm(ASTER_FONT_CAPTION_PX,buf);
 }
}
/* The baked ribbon is a 360x140 RGB565 image. It used to come out of the
 * LVGL pool via lv_snapshot_take(); five of them do not fit in the widget
 * pool, so the pixels live in the shared PSRAM heap and only the small
 * descriptor stays in the pool. */
static void ribbon_art_drop(lv_draw_buf_t *art)
{
 if(!art) return;
 aster_flux_draw_forget(art->data);
 aster_psram_free(art->unaligned_data);
 lv_free(art);
}
static lv_draw_buf_t *ribbon_bake(lv_obj_t *obj)
{
 int32_t w,h;
 uint32_t stride;
 size_t bytes;
 void *px;
 lv_draw_buf_t *db;

 lv_obj_update_layout(obj);
 w=lv_obj_get_width(obj);
 h=lv_obj_get_height(obj);
 if(w<=0 || h<=0) return NULL;
 stride=lv_draw_buf_width_to_stride((uint32_t)w,LV_COLOR_FORMAT_RGB565);
 bytes=(size_t)stride*(size_t)h;
 px=aster_psram_alloc(bytes);
 db=lv_malloc_zeroed(sizeof(*db));
 if(!px || !db ||
    lv_draw_buf_init(db,(uint32_t)w,(uint32_t)h,LV_COLOR_FORMAT_RGB565,stride,px,(uint32_t)bytes)!=LV_RESULT_OK ||
    lv_snapshot_take_to_draw_buf(obj,LV_COLOR_FORMAT_RGB565,db)!=LV_RESULT_OK) {
  LOG_WRN("ribbon bake failed (%dx%d)",(int)w,(int)h);
  aster_psram_free(px);
  if(db) lv_free(db);
  return NULL;
 }
 return db;
}
static void release_icon(int i)
{
 if(ribbons[i].art) {ribbon_art_drop(ribbons[i].art);ribbons[i].art=NULL;}
 if(ribbons[i].data) {
  aster_psram_free(ribbons[i].data);ribbons[i].data=NULL;
 }
}
static bool load_icon_locked(int i,const char *id)
{
 char dir[192],path[224];struct fs_dirent st;struct fs_file_t f;
 if(aster_storage_app_code_dir(dir,sizeof(dir),id))return false;
 snprintf(path,sizeof(path),"%s/icon.bin",dir);
 if(fs_stat(path,&st) || st.size<ASTER_ICON_HDR_SIZE || st.size>65536) return false;
 uint8_t *data=aster_psram_alloc(st.size);if(!data) return false;
 fs_file_t_init(&f);int err=fs_open(&f,path,FS_O_READ);
 if(!err) {err=aster_fs_read_safe(&f,data,st.size);fs_close(&f);}
 aster_icon_hdr_t h;
 if(err<0 || (size_t)err!=st.size || aster_icon_parse(data,st.size,&h) || h.w>128 || h.h>128) {
  aster_psram_free(data);return false;
 }
 lv_color_format_t format=h.cf==ASTER_ICON_CF_A8?LV_COLOR_FORMAT_A8:
  h.cf==ASTER_ICON_CF_RGB565?LV_COLOR_FORMAT_RGB565:
  h.cf==ASTER_ICON_CF_RGB565A8?LV_COLOR_FORMAT_RGB565A8:LV_COLOR_FORMAT_UNKNOWN;
 if(format==LV_COLOR_FORMAT_UNKNOWN) {aster_psram_free(data);return false;}
 ribbons[i].data=data;
 ribbons[i].icon=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=format,.w=h.w,.h=h.h,.stride=h.w*(h.cf==ASTER_ICON_CF_A8?1:2)},.data_size=h.data_size,.data=data+ASTER_ICON_HDR_SIZE};
 return true;
}
static bool load_icon(int i,const char *id)
{
 /* Keep an icon read on the generation selected by the registry, including
  * the short interval in which an installer publishes and rescans. */
 aster_app_mgr_files_lock();
 bool loaded=load_icon_locked(i,id);
 aster_app_mgr_files_unlock();
 return loaded;
}

static void bind_ribbon(int slot,int idx)
{
 bool same=ribbons[slot].entry==idx;
 if(same && !ribbons[slot].dirty_art) return;
 lv_image_set_src(ribbons[slot].obj,NULL);
 if(same) {
  if(ribbons[slot].art) ribbon_art_drop(ribbons[slot].art);
  ribbons[slot].art=NULL;
 } else release_icon(slot);
 ribbons[slot].entry=idx;ribbons[slot].dirty_art=false;
 entry_t *e=&entries[idx];
 /* Bake text, color and original icon once per entry/font arrival.
  * Move/rotate the resulting RGB565 image directly: no nested label
  * layers and no re-rasterizing a 360x140 object tree on every drag. */
 design_px=1;
 lv_obj_t *o=box(stage,0,0,360,140,e->color,0);
 char meta[64],code[96];int number=0;
 for(int i=0;i<count;i++) if(ids[i]==idx) number=i+1;
 snprintf(meta,sizeof(meta),"%02d / %s",number,e->code);
 text(o,meta,43,12,240,INK,16,128);
 if(!e->builtin && ((same && ribbons[slot].data) || load_icon(slot,e->id))) {
  lv_obj_t *img=lv_image_create(o);lv_image_set_src(img,&ribbons[slot].icon);
  lv_image_set_pivot(img,0,0);lv_image_set_scale(img,56*256/ribbons[slot].icon.header.w);
  lv_obj_set_pos(img,ax(43),ay(43));
  if(ribbons[slot].icon.header.cf==LV_COLOR_FORMAT_A8) {
   lv_obj_set_style_image_recolor(img,lv_color_hex(INK),0);lv_obj_set_style_image_recolor_opa(img,LV_OPA_COVER,0);
  }
 } else icon(o,e->icon,51,51,40,INK);
 /* System titles use their UI font. Installed app titles are always
  * package metadata rendered by the generic SD face; no app glyphs need
  * to be added to, or compiled into, the firmware. */
 if(e->builtin) text(o,e->name,112,43,190,INK,20,384);
 else {
  lv_obj_t *lab=lv_label_create(o);
  aster_ui_font_warm(ASTER_FONT_UI_PX,e->name);
  lv_obj_set_style_text_font(lab,aster_ui_font(ASTER_FONT_UI_PX),0);
  lv_obj_set_style_text_color(lab,lv_color_hex(INK),0);
  lv_label_set_long_mode(lab,LV_LABEL_LONG_DOT);
  lv_obj_set_pos(lab,112,51);lv_obj_set_width(lab,190);
  lv_label_set_text(lab,e->name);
  lv_obj_clear_flag(lab,LV_OBJ_FLAG_CLICKABLE);
 }
 icon(o,ICON_ARROW,306,56,21,INK);
 snprintf(code,sizeof(code),"ASTER / %s",e->id);
 for(char *p=code;*p;p++) if(*p>='a' && *p<='z') *p-=32;
 text(o,code,43,113,274,INK,16,128);
 box(o,310,0,1,140,0x504354,0);box(o,323,0,1,140,0x504354,0);
 ribbons[slot].art=ribbon_bake(o);
 design_px=0;
 lv_obj_delete(o);
 if(ribbons[slot].art) {
  aster_flux_draw_prepare(ribbons[slot].art->data,ribbons[slot].art->header.stride);
  lv_image_set_src(ribbons[slot].obj,ribbons[slot].art);
 }
}
static void set_sd_text(lv_obj_t *o,uint8_t px,const char *utf8)
{
 if(!o || !utf8) return;
 const lv_font_t *font=aster_ui_font(px);
 if(!strcmp(lv_label_get_text(o),utf8) && lv_obj_get_style_text_font(o,0)==font) return;
 aster_ui_font_warm(px,utf8);
 lv_obj_set_style_text_font(o,font,0);lv_label_set_text(o,utf8);
}
static void caption(void)
{
 entry_t *e=current();char buf[64];
 const char *desc=e?e->description:"这个频段还没有安装应用。";
 if(e && !e->builtin) set_sd_text(description,ASTER_FONT_CAPTION_PX,desc);
 else set_text(description,desc);
 snprintf(buf,sizeof(buf),"%02d / %02d",count?selected[group]+1:0,count);set_text(position,buf);
 snprintf(buf,sizeof(buf),"拉开%s",e?e->name:"应用");
 if(e && !e->builtin) set_sd_text(pull_label,ASTER_FONT_CAPTION_PX,buf);else set_text(pull_label,buf);
 /* One-line captions fit the gap above the handle even on 240x320.
  * AUTO height would wrap long package text over the launch controls. */
 lv_obj_set_height(description,lv_font_get_line_height(lv_obj_get_style_text_font(description,0)));
 lv_obj_set_height(pull_label,lv_font_get_line_height(lv_obj_get_style_text_font(pull_label,0)));
 /* Both native UI and APP captions center using their actual line height.
  * The label stays between the handle and arrow as the viewport changes. */
 lv_obj_set_style_text_align(pull_label,LV_TEXT_ALIGN_CENTER,0);
 lv_obj_center(pull_label);
 set_text(filter_label,group_names[group]);
}
/* Keep entries that remain in the five-ribbon window. Crossing an index
 * replaces only the one newcomer, not every slot's text/icon texture. */
static int cache_slot(const int resident[RIBBONS],const int *wanted,int n,int entry)
{
 for(int k=0;k<RIBBONS;k++) if(resident[k]==entry) return k;
 for(int k=0;k<RIBBONS;k++) {
  bool keep=false;
  for(int i=0;i<n;i++) if(resident[k]==wanted[i]) keep=true;
  if(!keep) return k;
 }
 return -1;
}
static void ribbon_order(const int index[RIBBONS])
{
 int order[RIBBONS],n=0;
 /* Hidden/unused images stay behind the visible cards. Preserve the
  * original far-to-near order and slot tie-break at selection crossings. */
 for(int k=0;k<RIBBONS;k++)
  if(index[k]<0 || abs(distance(index[k],phase,count))/1000>3) order[n++]=k;
 for(int threshold=3;threshold>=0;threshold--) for(int k=0;k<RIBBONS;k++)
  if(index[k]>=0 && abs(distance(index[k],phase,count))/1000==threshold) order[n++]=k;
 /* Moving every image to the foreground invalidates the whole stage
  * even when the final order is identical. Only move changed siblings.
  * The FLUX watermark remains below the five ribbon objects. */
 int base=(int)lv_obj_get_child_count(stage)-RIBBONS;
 for(int i=0;i<n;i++) lv_obj_move_to_index(ribbons[order[i]].obj,base+i);
}
static void redraw(void)
{
 draw_pending=false;
 if(!stage || page!=HOME) return;
 int center=nearest(phase),wanted[RIBBONS],n=0;
 for(int rel=-2;rel<=2 && count;rel++) {
  int idx=ids[wrap(center+rel,count)];bool duplicate=false;
  for(int i=0;i<n;i++) if(wanted[i]==idx) duplicate=true;
  if(!duplicate) wanted[n++]=idx;
 }
 for(int i=0;i<n;i++) {
  int resident[RIBBONS];for(int k=0;k<RIBBONS;k++) resident[k]=ribbons[k].entry;
  int slot=cache_slot(resident,wanted,n,wanted[i]);
  if(slot>=0) bind_ribbon(slot,wanted[i]);
 }
 int index[RIBBONS];
 for(int k=0;k<RIBBONS;k++) {
  index[k]=-1;lv_obj_t *o=ribbons[k].obj;
  for(int i=0;i<count;i++) if(ids[i]==ribbons[k].entry) index[k]=i;
  int idx=index[k],d=idx<0?INT32_MAX:distance(idx,phase,count);
  int a=abs(d);
  if(idx<0 || a>=2400 || !ribbons[k].art) {lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);continue;}
  lv_obj_clear_flag(o,LV_OBJ_FLAG_HIDDEN);
  int shift=idx==selected[group]?pull:0;
  int zoom=MAX(174,256-a*41/1000);
  int f=fit256();
  lv_image_set_rotation(o,wrap(-90+CLAMP(d*60/1000,-120,120),3600));
  if(f==256) {
   lv_obj_set_pos(o,-20+a*9/1000+shift,126+d*FLUX_RIBBON_PITCH/1000-70);
   lv_image_set_pivot(o,180,70);
   lv_image_set_scale(o,zoom);
  } else {
   int design_x=-20+a*9/1000+shift;
   int design_y=126+d*FLUX_RIBBON_PITCH/1000-70;
   int sc=zoom*f/256; if(sc<1) sc=1;
   lv_obj_set_pos(o,design_x*f/256-180+180*f/256,design_y*f/256-70+70*f/256);
   lv_image_set_pivot(o,180,70);
   lv_image_set_scale(o,(uint32_t)sc);
  }
  lv_obj_set_style_image_opa(o,MAX(46,255-a*97/1000),0);
 }
 ribbon_order(index);
 lv_obj_set_x(handle,ax(3+MIN(230,pull)));
 last_draw=k_uptime_get();
 aster_ui_lvgl_request_frame();
}
static void settle(int target)
{
 selected[group]=wrap(nearest(target),count);
 from_phase=phase;target_phase=phase+distance(selected[group],phase,count);
 from_pull=pull;scroll_duration=FLUX_SNAP_MS;coasting=false;
 animation_ms=k_uptime_get();animating=motion && (target_phase!=phase || pull!=0);caption();save();
 if(!motion) {phase=target_phase;pull=0;draw_pending=true;}
}
static void release_scroll(uint64_t now)
{
 struct flux_scroll_plan p=flux_scroll_plan(phase,flux_velocity_release(&drag.track,now),count);
 if(!motion || !p.coast) {settle(phase);return;}
 from_phase=phase;target_phase=p.target;from_pull=pull;
 scroll_duration=p.duration;animation_ms=now;coasting=true;animating=true;
 selected[group]=wrap(nearest(phase),count);caption();save();
 LOG_INF("flux coast: speed=%d px/s travel=%d duration=%u ms",drag.track.speed,target_phase-from_phase,scroll_duration);
}
static void slider_changed(lv_event_t *e)
{
 int kind=(int)(intptr_t)lv_event_get_user_data(e);
 int v=lv_slider_get_value(lv_event_get_target(e));
 if(kind==0) {brightness=v;brightness_pending=v;} else volume_pending=v;
 lv_obj_t *l=lv_obj_get_user_data(lv_event_get_target(e));char value[16];
 snprintf(value,sizeof(value),"%d%%",v);set_text(l,value);
 if(lv_event_get_code(e)==LV_EVENT_RELEASED) {
  save();
  if(kind==1) volume_save=true;
 }
}
static void slider_card(int y,const char *name,const char *hint,uint32_t color,int value,int kind)
{
 lv_obj_t *card=box(body,17,y,286,111,0x191f18,17);
 text(card,name,15,10,60,color,20,280);
 char buf[16];snprintf(buf,sizeof(buf),"%d%%",value);
 lv_obj_t *out=text(card,buf,205,16,65,DIM,16,220);
 lv_obj_t *s=lv_slider_create(card);lv_obj_set_pos(s,ax(15),ay(55));lv_obj_set_size(s,ax(250),ay(8));
 lv_slider_set_range(s,kind==0?25:0,100);lv_slider_set_value(s,value,LV_ANIM_OFF);
 lv_obj_set_style_bg_color(s,lv_color_hex(0x394330),LV_PART_MAIN);
 lv_obj_set_style_bg_color(s,lv_color_hex(color),LV_PART_INDICATOR);
 lv_obj_set_style_bg_color(s,lv_color_hex(color),LV_PART_KNOB);
 lv_obj_set_style_pad_all(s,as(5),LV_PART_KNOB);
 lv_obj_set_user_data(s,out);
 lv_obj_add_event_cb(s,slider_changed,LV_EVENT_VALUE_CHANGED,(void *)(intptr_t)kind);
 lv_obj_add_event_cb(s,slider_changed,LV_EVENT_RELEASED,(void *)(intptr_t)kind);
 text(card,hint,15,82,250,0x6c8356,16,160);
}
static void build_home(void)
{
 bool lckfb=IS_ENABLED(CONFIG_ASTER_HW_LCKFB_S3);
 text(body,"流光",16,0,110,FG,20,320);
 lv_obj_t *filter=button(body,NULL,lckfb?236:250,lckfb?0:8,lckfb?68:53,lckfb?36:25,0x22261f,ACT_FILTERS);
 lv_obj_set_style_border_width(filter,1,0);lv_obj_set_style_border_color(filter,lv_color_hex(0x394330),0);
 filter_label=text(filter,group_names[group],9,5,lckfb?56:32,DIM,16,lckfb?288:160);
 if(lckfb) {
  lv_obj_set_style_text_align(filter_label,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_center(filter_label);
  lv_obj_set_ext_click_area(filter,4);
 }
 lv_obj_t *subtitle=text(body,"SCROLL THE MOMENT",17,38,190,0x91a184,16,128);
 position=text(body,"",lckfb?211:250,38,lckfb?92:55,0x91a184,16,128);
 stage=box(body,0,53,320,280,BG,0);
 if(lckfb) {
  /* Native glyphs keep their line height when the design spacing shrinks.
   * Reserve the entire header row before clipping the ribbon stage. */
  int line_h=lv_font_get_line_height(lv_obj_get_style_text_font(subtitle,0));
  int stage_top=MAX(ay(53),ay(38)+line_h+as(6));
  lv_obj_set_y(stage,stage_top);
  lv_obj_set_height(stage,MAX(1,ay(333)-stage_top));
  lv_obj_set_style_text_align(position,LV_TEXT_ALIGN_RIGHT,0);
 }
 text(stage,"FLUX",18,3,130,0x24202c,20,600);
 for(int k=0;k<RIBBONS;k++) {
  ribbons[k].entry=-1;ribbons[k].obj=lv_image_create(stage);
  lv_obj_clear_flag(ribbons[k].obj,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(ribbons[k].obj,0,0);
  lv_image_set_antialias(ribbons[k].obj,true);
 }
 lv_obj_t *rail=box(body,4,164,4,13,ACCENT,2);lv_obj_move_foreground(rail);
 box(body,312,173,4,4,ACCENT,2);
 description=text(body,"",17,335,215,0xa0ac91,16,160);
 icon(body,ICON_MOVE,243,337,11,0x91a184);
 text(body,"上下拨动",257,337,59,0x91a184,16,128);
 lv_obj_t *track=box(body,17,364,286,43,0x24291d,24);
 lv_obj_set_style_border_width(track,1,0);lv_obj_set_style_border_color(track,lv_color_hex(0x4b5934),0);
 box(track,52,21,190,1,0x485335,0);
 pull_label=text(track,"",65,13,178,0xa9bb8b,16,180);
 handle=box(track,3,3,40,35,ACCENT,20);icon(handle,ICON_RIGHT,11,9,18,0x334121);
 icon(track,ICON_ARROW,261,13,15,DIM);
 lv_obj_t *prev=button(body,NULL,16,410,29,26,BG,ACT_PREV);icon(prev,ICON_LEFT,8,6,13,0x819b64);
 lv_obj_t *next=button(body,NULL,275,410,29,26,BG,ACT_NEXT);icon(next,ICON_RIGHT,8,6,13,0x819b64);
 text(body,"SDK APP / ORIGINAL ICON",66,419,200,0x8b9c7a,16,112);
 caption();redraw();
}
static void build_filters(void)
{
 text(body,"TUNE YOUR FLOW",17,15,280,DIM,16,128);
 text(body,"选择一段流光",17,37,286,FG,20,280);
 text(body,"只改变浏览范围，不改变安装内容。",17,73,286,DIM,16,160);
 for(int g=0;g<4;g++) {
  int n=0;for(int j=0;j<entry_n;j++) if(!g || entries[j].group==g) n++;
  lv_obj_t *b=button(body,NULL,17,110+g*74,286,65,group_colors[g],ACT_GROUP+g);
  lv_obj_set_style_radius(b,3,0);
  text(b,group_codes[g],15,8,245,0x253223,16,128);
  text(b,group_names[g],15,29,90,0x253223,20,256);
  char count_text[32];snprintf(count_text,sizeof(count_text),"%d 个入口",n);
  text(b,count_text,98,35,138,0x46533d,16,160);
  if(g==group) box(b,257,27,7,7,0x253223,4);
  else icon(b,ICON_ARROW,253,24,18,0x253223);
 }
 text(body,"安装后自动加入光带，未知应用归入探索。",17,415,286,0x91a184,16,144);
}
static void build_mixer(void)
{
 text(body,"DEVICE MIXER",17,15,280,DIM,16,128);
 text(body,"调到你的节奏。",17,36,286,FG,20,280);
 const char *names[]={"Wi-Fi","蓝牙","静谧"};int acts[]={ACT_WIFI,ACT_NONE,ACT_QUIET};
 int art[]={ICON_WIFI,ICON_BT,ICON_MOON};
 for(int i=0;i<3;i++) {
  uint32_t color=(i==2 && quiet)?ACCENT:0x252d22;
  lv_obj_t *b=button(body,NULL,17+i*98,89,90,68,color,acts[i]);
  icon(b,art[i],12,8,19,color==ACCENT?0x394b20:DIM);
  text(b,names[i],12,30,70,color==ACCENT?0x394b20:FG,16,180);
  lv_obj_t *status=text(b,i==0?"读取状态":i==1?"未启用":quiet?"已开启":"已关闭",12,49,74,DIM,16,128);
  if(i==0) wifi_state=status;
  if(i==1) lv_obj_clear_flag(b,LV_OBJ_FLAG_CLICKABLE);
 }
 slider_card(172,"光","屏幕亮度",ACCENT,brightness,0);
 int volume=volume_pending>=0?volume_pending:aster_plat_audio_get_volume();slider_card(297,"声","输出音量",0xffb8a5,MAX(0,volume),1);
 icon(body,ICON_BATTERY,17,420,15,DIM);
 text(body,"USB 供电",38,422,220,DIM,16,160);
 lv_obj_t *settings=button(body,NULL,273,413,30,27,0x252d22,ACT_SYSTEM);
 icon(settings,ICON_SETTINGS,6,4,18,ACCENT);
}
static void build_files(void)
{
 text(body,"LOCAL STORAGE",17,15,280,DIM,16,128);
 text(body,"文件",17,38,286,FG,20,320);
 text(body,aster_storage_mounted()?"SD 卡 /SD:":"SD 卡未挂载",17,85,286,DIM,16,256);
 lv_obj_t *list=box(body,17,125,286,295,0x191d1a,12);
 lv_obj_add_flag(list,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(list,LV_DIR_VER);
 struct fs_dir_t d;fs_dir_t_init(&d);int row=0;
 if(!fs_opendir(&d,"/SD:")) {
  struct fs_dirent ent;
  while(row<24 && !fs_readdir(&d,&ent) && ent.name[0]) {
   char line[sizeof(ent.name)+16];snprintf(line,sizeof(line),"%s  %s",ent.type==FS_DIR_ENTRY_DIR?"+":"·",ent.name);
   text(list,line,12,12+row++*32,262,FG,16,220);
  }
  fs_closedir(&d);
 }
 if(!row) text(list,"没有可读取的文件。",12,18,262,DIM,16,220);
}
static void build_system(void)
{
 text(body,"SYSTEM / ASTER",17,15,280,DIM,16,128);
 text(body,"设备与运行状态",17,38,286,FG,20,280);
 char info[480];
 snprintf(info,sizeof(info),"%s\n\n显示   %u × %u\nABI     3.8.0\n应用   %d 个已安装\nSD       %s\n运行   %llu 秒\n\n启动与错误日志通过 USB shell 查看。",CONFIG_ASTER_BOARD_ID,(unsigned)aster_ui_lvgl_width(),(unsigned)aster_ui_lvgl_height(),aster_app_mgr_count(),aster_storage_mounted()?"已挂载":"未挂载",(unsigned long long)(k_uptime_get()/1000));
 text(body,info,17,100,286,FG,16,256);
}
static void show_page(int next)
{
 /* Desktop pages share one cache owner; keep hot glyphs until APP entry. */
 if(next!=HOME) {animating=false;coasting=false;phase=selected[group]*1000;pull=0;}
 for(int i=0;i<RIBBONS;i++) {ribbons[i].obj=NULL;ribbons[i].entry=-1;}
 if(body) lv_obj_delete(body);
 for(int i=0;i<RIBBONS;i++) release_icon(i);
 stage=description=position=pull_label=handle=filter_label=wifi_state=clock_value=clock_date=NULL;
 page=next;body=box(root,0,40,320,440,next==HOME?BG:0x181b18,0);
 lv_obj_t *brand=lv_obj_get_child(root,0);
 /* Top bar is independent of page destruction. */
 lv_obj_t *back=lv_obj_get_child(brand,0),*name=lv_obj_get_child(brand,1),*mark=lv_obj_get_child(brand,2);
 if(next==HOME) {lv_obj_add_flag(back,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(name,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(mark,LV_OBJ_FLAG_HIDDEN);}
 else {lv_obj_clear_flag(back,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(name,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(mark,LV_OBJ_FLAG_HIDDEN);}
 if(next==HOME) build_home();else if(next==FILTERS) build_filters();
 else if(next==MIXER) build_mixer();else if(next==FILES) build_files();
 else if(next==SYSTEM) build_system();else {
  text(body,"A MOMENT / CLOCK",17,15,286,DIM,16,128);
  clock_value=text(body,"--:--",47,130,256,FG,20,640);
  clock_date=text(body,"",47,230,256,DIM,16,256);
 }
 clock_sec=UINT64_MAX;lv_obj_invalidate(root);
}
static void open_current(void)
{
 entry_t *e=current();if(!e) return;
 if(e->builtin) {prior_page=HOME;show_page(e->builtin);return;}
 int err=aster_runtime_launch_async(e->id);
 LOG_INF("flux: launch %s -> %d",e->id,err);
 if(err) {launching=false;pull=0;notify("应用启动失败，请查看日志。 ");redraw();}
}
static void removal(void)
{
 entry_t *e=current();if(!e || e->builtin) return;
 snprintf(remove_id,sizeof(remove_id),"%s",e->id);snprintf(remove_name,sizeof(remove_name),"%s",e->name);
 modal=box(root,0,0,320,480,0x000000,0);lv_obj_set_style_bg_opa(modal,LV_OPA_70,0);
 lv_obj_add_flag(modal,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_t *card=box(modal,17,113,286,252,0x242a20,17);
 text(card,"卸载应用",16,18,254,ACCENT,20,256);
 text(card,remove_name,16,62,254,FG,20,256);
 text(card,"应用和目录数据将删除。\n此操作无法撤销。",16,108,254,DIM,16,230);
 button(card,"取消",16,190,118,43,0x343e2b,ACT_CANCEL);
 button(card,"卸载",151,190,118,43,0x5d3933,ACT_UNINSTALL);
 LOG_INF("flux: uninstall prompt %s",remove_id);
}
static int ribbon_hit(int x,int y)
{
 int found=-1,best=INT32_MAX;
 for(int k=0;k<RIBBONS;k++) {
  lv_obj_t *o=ribbons[k].obj;
  if(!o || !ribbons[k].art || lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN)) continue;
  lv_area_t area;lv_obj_get_coords(o,&area);
  int bw=ribbons[k].art->header.w,bh=ribbons[k].art->header.h;
  lv_point_t p={x-area.x1,y-area.y1},pivot={bw/2,bh/2};
  int zx=lv_image_get_scale_x(o),zy=lv_image_get_scale_y(o);
  if(zx<1 || zy<1) continue;
  lv_point_transform(&p,-lv_image_get_rotation(o),65536/zx,65536/zy,&pivot,false);
  if(p.x<0 || p.x>=bw || p.y<0 || p.y>=bh) continue;
  for(int i=0;i<count;i++) if(ids[i]==ribbons[k].entry) {
   int d=abs(distance(i,phase,count));
   if(d<best) {best=d;found=i;}
  }
 }
 return found;
}
/* Reuse the physical edge thresholds: top pull opens the mixer; bottom
 * pull closes it. Slider strokes start outside the bottom band. Claiming a
 * pull suppresses widget clicks; the launcher never maps it to APP exit. */
static bool header_touch(const aster_touch_evt_t *raw)
{
 bool pressed=raw->action!=ASTER_TOUCH_UP;
 bool enabled=visible && !modal && !aster_wifi_settings_is_open() &&
              !aster_qpanel_is_open() && !launching;
 uint32_t allow=page==MIXER?ASTER_GEST_ALLOW_EXIT:ASTER_GEST_ALLOW_PANEL;
 aster_gest_t g=aster_gesture_feed(raw->x,raw->y,pressed,enabled?allow:0,view_h);
 if(!enabled || (page==MIXER?g!=ASTER_GEST_PANEL_UP:g!=ASTER_GEST_PANEL_DOWN)) return false;
 aster_ui_lvgl_pointer(raw->x,raw->y,pressed);
 lv_indev_wait_release(aster_ui_lvgl_indev());
 memset(&drag,0,sizeof(drag));
 action=ACT_MIXER;
 return true;
}
static void touch(const aster_touch_evt_t *raw)
{
 if(header_touch(raw)) return;
 aster_touch_evt_t mapped=*raw;
 to_design(&mapped.x,&mapped.y);
 const aster_touch_evt_t *e=&mapped;
 bool pressed=e->action!=ASTER_TOUCH_UP;
 bool owned=visible && page==HOME && !modal && !aster_wifi_settings_is_open();
 if(e->action==ASTER_TOUCH_DOWN) {
  memset(&drag,0,sizeof(drag));
  if(owned && ((e->y>=93 && e->y<373) || (e->y>=404 && e->y<=447))) {
   aster_ui_lvgl_pointer(raw->x,raw->y,false);
   drag.down=true;drag.handle=e->y>=404;drag.x=e->x;drag.y=e->y;
   drag.hit=drag.handle?-1:ribbon_hit(raw->x,raw->y);
   drag.stopped=animating && !drag.handle;
   drag.base=phase;drag.started=k_uptime_get();animating=false;coasting=false;
   selected[group]=wrap(nearest(phase),count);caption();
   flux_velocity_reset(&drag.track,e->y,drag.started);
  }
 }
 if(!drag.down) {aster_ui_lvgl_pointer(raw->x,raw->y,pressed);return;}
 int dx=(int)e->x-drag.x,dy=(int)e->y-drag.y;
 if(!drag.axis && dx*dx+dy*dy>36) {
  drag.axis=drag.handle?1:abs(dy)>=6?2:3;drag.moved=true;
 }
 /* Horizontal jitter on a card cannot claim the launch gesture. */
 if(!drag.handle && drag.axis==3 && abs(dy)>=6) drag.axis=2;
 if(!drag.blocked) {
  if(drag.axis==2) {
   flux_velocity_sample(&drag.track,e->y,k_uptime_get());
   phase=drag.base-dy*1000/FLUX_RIBBON_PITCH;
   {int sel=wrap(nearest(phase),count);
    if(sel!=selected[group]) {selected[group]=sel;caption();}}
   draw_pending=true;
  } else if(drag.axis==1 && pressed) {pull=CLAMP(dx,0,240);draw_pending=true;}
 }
 if(!pressed) {
  if(!drag.blocked) {
   if(drag.axis==2) {
    release_scroll(k_uptime_get());
   } else if(drag.handle && drag.axis==1 && dx>95 && abs(dy)<24) {
    launching=true;animation_ms=k_uptime_get();from_pull=pull;
   } else if(drag.moved || drag.stopped) settle(nearest(phase)*1000);
   else if(k_uptime_get()-drag.started>300) settle(nearest(phase)*1000);
   else if(drag.handle) action=ACT_OPEN;
   else {
    if(drag.hit>=0 && ribbon_hit(raw->x,raw->y)==drag.hit) {
     if(drag.hit==selected[group]) action=ACT_OPEN;else settle(drag.hit*1000);
    }
   }
  }
  drag.down=false;
 }
}
static void tick_clock(void)
{
 uint64_t sec=k_uptime_get()/1000;if(sec==clock_sec) return;clock_sec=sec;
 aster_time_info_t t={0};char value[48];
 if(!aster_time_read(&t)) snprintf(value,sizeof(value),"%02u:%02u",t.hour,t.minute);
 else snprintf(value,sizeof(value),"--:--");
 set_text(bar_time,value);set_text(clock_value,value);
 snprintf(value,sizeof(value),"%04d-%02u-%02u%s",t.year,t.month,t.day,(t.flags&ASTER_TIME_FLAG_SYNCED)?"":" · 默认时间");set_text(clock_date,value);
 if(wifi_state) {
  aster_wifi_status_t s={0};int err=aster_plat_wifi_status(&s);
  set_text(wifi_state,err || !s.ip[0]?"未连接":"已连接");
 }
}
static void palette(aster_qpanel_palette_t *p)
{ *p=(aster_qpanel_palette_t){.bg=0x181b18,.fg=FG,.dim=DIM,.surface=0x252d22,.accent=ACCENT}; }
static const aster_qpanel_theme_ops_t theme={.palette=palette};
static struct {bool down,edge;int32_t x,y;} app_drag;
bool aster_launcher_app_touch(int32_t x,int32_t y,bool pressed)
{
 if(pressed && !app_drag.down) {
  app_drag.down=true;app_drag.x=x;app_drag.y=y;
  app_drag.edge=x>=(int32_t)aster_ui_lvgl_width()-22;
 } else if(!pressed) {
  bool back=app_drag.down && app_drag.edge && x-app_drag.x<-65 &&
            abs(x-app_drag.x)>abs(y-app_drag.y);
  memset(&app_drag,0,sizeof(app_drag));return back;
 }
 return false;
}
int aster_launcher_init(void)
{
 if(initialized) return 0;
 initialized=true;
 for(size_t face=0;face<ARRAY_SIZE(flux_icon_sizes);face++) for(size_t i=0;i<ARRAY_SIZE(chrome[face]);i++) {
  unsigned px=flux_icon_sizes[face];
  chrome[face][i]=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,.w=px,.h=px,.stride=px},.data_size=px*px,.data=flux_icon_data[face][i]};
 }
 for(int i=0;i<RIBBONS;i++) ribbons[i].entry=-1;
 aster_app_mgr_set_change_cb(aster_launcher_on_registry_change);aster_qpanel_bind_theme(&theme);
 uint8_t s[8];size_t n=0;
 if(!aster_kv_get(KV_ID,"state",s,sizeof(s),&n) && n==sizeof(s)) {
  group=MIN(s[0],3);for(int i=0;i<4;i++) selected[i]=s[i+1];brightness=CLAMP(s[5],25,100);motion=s[6]!=0;quiet=s[7]!=0;
 }
 return 0;
}
void aster_launcher_show(void)
{
 if(visible || !aster_ui_lvgl_screen()) return;
 aster_launcher_init();aster_qpanel_force_close();aster_gesture_reset();
 if(aster_plat_display_orientation_get()!=ASTER_ORIENT_PORTRAIT) (void)aster_plat_display_orientation_set(ASTER_ORIENT_PORTRAIT);
 aster_ui_lvgl_resync_window();
 view_w=(int)aster_ui_lvgl_width();view_h=(int)aster_ui_lvgl_height();
 if(view_w<1) view_w=320;
 if(view_h<1) view_h=480;
 aster_ui_fonts_prepare(view_w,view_h,"流光");
 lv_obj_clean(aster_ui_lvgl_screen());
 aster_ui_lvgl_pointer(0,0,0);lv_indev_reset(aster_ui_lvgl_indev(),NULL);
 read_entries();filter_entries();warm_visible_text();visible=true;dirty=false;launching=false;memset(&drag,0,sizeof(drag));
 memset(&app_drag,0,sizeof(app_drag));
 root=box(aster_ui_lvgl_screen(),0,0,320,480,BG,0);
 lv_obj_t *bar=box(root,0,0,320,40,BG,0);
 lv_obj_t *back=button(bar,NULL,10,5,75,29,BG,ACT_BACK);
 icon(back,ICON_LEFT,9,7,14,FG);
 text(back,"收回",29,6,40,FG,16,256);
 lv_obj_set_style_bg_opa(back,LV_OPA_TRANSP,0);
 bool lckfb=IS_ENABLED(CONFIG_ASTER_HW_LCKFB_S3);
 lv_obj_t *brand_name=text(bar,"aster",lckfb?36:32,lckfb?0:11,105,0xd4e6b2,16,lckfb?288:192);
 lv_obj_t *brand_mark=icon(bar,ICON_ASTER,15,lckfb?0:13,lckfb?18:12,0xd4e6b2);
 if(lckfb) {
  lv_obj_align(brand_name,LV_ALIGN_LEFT_MID,ax(36),0);
  lv_obj_align(brand_mark,LV_ALIGN_LEFT_MID,ax(15),0);
 }
 /* The rightmost 48 design pixels are reserved for future battery status. */
 bar_time=text(bar,"--:--",205,lckfb?0:14,60,DIM,16,lckfb?288:160);
 if(lckfb) {
  lv_obj_align(bar_time,LV_ALIGN_LEFT_MID,ax(205),0);
 }
 body=NULL;show_page(HOME);tick_clock();
 aster_qpanel_restore_volume();brightness_pending=brightness;
 saved_volume=aster_plat_audio_get_volume();
 if(quiet) volume_pending=0;
 sound_entry=current()?(int)(current()-entries):-1;sound_group=group;
 sound_phase=phase;sound_sample_ms=k_uptime_get();sound_launch_started=false;
 if(sound_return) {sound_pending=S_CLOSE;sound_return=false;}
 LOG_INF("FLUX Rev.04: 320x480, %d entries (%d installed)",entry_n,aster_app_mgr_count());
}
void aster_launcher_hide(void)
{
 if(!visible) return;
 aster_qpanel_force_close();save_now();aster_gesture_reset();
 aster_plat_audio_ui_release();
 lv_obj_clean(aster_ui_lvgl_screen());
 for(int i=0;i<RIBBONS;i++) {release_icon(i);ribbons[i].obj=NULL;}
 sound_return=true;
 root=body=stage=bar_time=modal=toast=NULL;visible=false;hold_ms=0;clock_sec=UINT64_MAX;
}
void aster_launcher_request_show(void) {want_show=true;}
void aster_launcher_on_registry_change(void) {dirty=true;}
void aster_launcher_show_page(int p) {want_page=p?FILTERS:HOME;}
void aster_launcher_step_page(int dir) {if(visible && page==HOME) settle((selected[group]+(dir>0?1:-1))*1000);}
uint32_t aster_launcher_idle_wait_ms(void)
{
 return visible && (drag.down || animating || launching)?1:10;
}
void aster_launcher_pump_idle(void)
{
 if(!aster_plat_display_count()) return;
 if(aster_ui_fonts_poll() && aster_ui_lvgl_screen()) {
  /* Rebaking during a flick is the hitch between cards. Glyphs that
   * arrive mid-gesture wait until the ribbon has settled. */
  if(stage && page==HOME && !drag.down && !animating && !launching) {
   for(int i=0;i<RIBBONS;i++) ribbons[i].dirty_art=true;
   redraw();
  }
  lv_obj_invalidate(aster_ui_lvgl_screen());
 }
 if(!visible && want_show) {
  uint64_t now=k_uptime_get();if(!hold_ms) hold_ms=now?now:1;
  /* Stay on the splash until the SD face can accept a warm, the installed
   * names and descriptions have been queued, and those bitmaps have landed.
   * Marking the catalog warm before the face is READY used to skip the
   * animation entirely. */
  if(!aster_font_sd_streaming() && aster_font_sd_get(ASTER_FONT_UI_PX) && !catalog_warmed) {
   read_entries();catalog_warmed=1;
  }
  if((aster_font_sd_streaming() || !aster_font_sd_get(ASTER_FONT_UI_PX) ||
      aster_font_sd_busy() || !catalog_warmed) && now-hold_ms<12000) {
   aster_splash_boot();aster_splash_tick();return;
  }
  aster_splash_off();aster_launcher_show();
 }
 if(!visible) return;
 if(dirty && !modal) {aster_launcher_hide();sound_return=false;aster_launcher_show();}
 aster_touch_evt_t events[ASTER_TOUCH_MAX];int n=aster_plat_input_poll(events,ASTER_TOUCH_MAX);
 for(int i=0;i<n;i++) touch(&events[i]);
 aster_key_evt_t keys[ASTER_PLAT_MAX_KEYS];while(aster_plat_input_keys_poll(keys,ASTER_PLAT_MAX_KEYS)>0) {}
 uint64_t now=k_uptime_get();
 if(drag.down && !drag.moved && !drag.handle && !drag.blocked && now-drag.started>=650 && drag.hit==selected[group] && current() && !current()->builtin) {
  drag.blocked=true;removal();
 }
 int a=action;action=ACT_NONE;
 if(a>=ACT_GROUP && a<ACT_GROUP+4) {group=a-ACT_GROUP;filter_entries();show_page(HOME);save();sound_pending=S_TUNE;}
 else if(a==ACT_BACK) {if(page==SYSTEM && prior_page==MIXER) {prior_page=HOME;show_page(MIXER);} else show_page(HOME);sound_pending=S_CLOSE;}
 else if(a==ACT_FILTERS) {show_page(FILTERS);sound_pending=S_TAP;}
 else if(a==ACT_MIXER) {prior_page=HOME;sound_pending=page==MIXER?S_CLOSE:S_TAP;show_page(page==MIXER?HOME:MIXER);}
 else if(a==ACT_PREV || a==ACT_NEXT) settle((selected[group]+(a==ACT_PREV?-1:1))*1000);
 else if(a==ACT_OPEN) {launching=true;animation_ms=now;from_pull=pull;}
 else if(a==ACT_SYSTEM) {prior_page=MIXER;show_page(SYSTEM);sound_pending=S_TAP;}
 else if(a==ACT_WIFI) {aster_wifi_settings_open(root,NULL);sound_pending=S_TAP;}
 else if(a==ACT_QUIET) {
  quiet=!quiet;
  if(quiet) {saved_volume=aster_plat_audio_get_volume();volume_pending=0;} else volume_pending=saved_volume;
  if(quiet) aster_plat_audio_ui_stop();else sound_pending=S_TAP;
  show_page(MIXER);save();
 } else if(a==ACT_MOTION) {motion=!motion;save();sound_pending=S_TAP;}
 else if(a==ACT_CANCEL && modal) {lv_obj_delete(modal);modal=NULL;}
 else if(a==ACT_UNINSTALL && modal) {
  char id[64];snprintf(id,sizeof(id),"%s",remove_id);lv_obj_delete(modal);modal=NULL;
  int err=aster_app_mgr_uninstall(id);LOG_INF("flux: uninstall %s -> %d",id,err);
  if(err) notify("卸载失败，请查看系统日志。");
 }
 if(want_page>=0) {int p=want_page;want_page=-1;show_page(p);}
 if(animating && now-last_draw>=FLUX_FRAME_MS) {
  int ease=flux_scroll_ease(now-animation_ms,scroll_duration,coasting);
  phase=from_phase+(target_phase-from_phase)*ease/1000;pull=from_pull*(1000-ease)/1000;
  if(coasting) {
   int index=wrap(nearest(phase),count);
   if(index!=selected[group]) {selected[group]=index;caption();}
  }
  if(now-animation_ms>=scroll_duration) {
   animating=false;coasting=false;selected[group]=wrap(nearest(target_phase),count);
   phase=selected[group]*1000;pull=0;caption();save();
  }
  draw_pending=true;
 }
 if(!launching) sound_launch_started=false;
 if(launching && !sound_launch_started) {sound_pending=S_OPEN;sound_launch_started=true;}
 if(sound_pending>=0 && volume_pending<0) {sound_play(sound_pending,0);sound_pending=-1;}
 if(launching && page==HOME && now-last_draw>=FLUX_FRAME_MS) {
  int t=motion?MIN(1000,(int)(now-animation_ms)*1000/350):1000;
  pull=from_pull+(370-from_pull)*t/1000;redraw();
  if(t==1000) {launching=false;pull=0;open_current();}
 }
 if(draw_pending) redraw();
 if(brightness_pending>=0) {(void)aster_plat_backlight_set(true,brightness_pending*255/100);brightness_pending=-1;}
 if(volume_pending>=0) {(void)aster_plat_audio_set_volume(volume_pending);volume_pending=-1;}
 if(volume_save) {
  uint8_t value=CLAMP(aster_plat_audio_get_volume(),0,100);
  (void)aster_kv_set("aster.qpanel","volume",&value,1);volume_save=false;
 }
 int sound_now=current()?(int)(current()-entries):-1;
 if(sound_pending>=0) {sound_play(sound_pending,0);sound_pending=-1;}
 else if(visible && page==HOME && !launching && sound_group==group && sound_now!=sound_entry) {
  int speed=drag.down?drag.track.speed:
   now>sound_sample_ms?(int)((int64_t)(phase-sound_phase)*FLUX_RIBBON_PITCH/(now-sound_sample_ms)):0;
  sound_play(S_TICK,speed);
 }
 sound_entry=sound_now;sound_group=group;sound_phase=phase;sound_sample_ms=now;
 if(toast && now>=toast_until) {lv_obj_delete(toast);toast=NULL;}
 if(save_due && now>=save_due && !drag.down && !animating && !launching) save_now();
 tick_clock();aster_wifi_settings_tick();aster_qpanel_tick();aster_ui_lvgl_pump();
}
