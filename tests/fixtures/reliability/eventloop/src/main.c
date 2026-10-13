#define TEST_MODE 2
#define TEST_VERSION 2
#include <aster/aster.h>
#include "aster_printf.h"
#include "aster_gfx_engine.h"
#include "aster_libc.h"
static aster_handle_t surface;
static aster_ge_t ge;static aster_gfx_cmd_t commands[8];static uint8_t arena[256];
static int frame;
static int seed(const char *path,const char *value) {
 aster_handle_t f;char buf[64];int n=aster_fs_open(path,strlen(path),ASTER_FS_O_RDONLY,&f);
 if(n==ASTER_ENOENT) {
  n=aster_fs_open(path,strlen(path),ASTER_FS_O_WRONLY|ASTER_FS_O_CREAT,&f);
  if(n<0)return n;
  n=aster_fs_write(f,value,strlen(value));int c=aster_fs_close(f);
  return n==(int)strlen(value)&&!c?0:ASTER_EIO;
 }
 if(n<0)return n;
 n=aster_fs_read(f,buf,sizeof(buf));int c=aster_fs_close(f);
 return n==(int)strlen(value)&&!memcmp(buf,value,n)&&!c?0:ASTER_ECORRUPT;
}
static int prepare(void) {
 int rc=seed("/data/save.txt","SAVE_KEEP_123\n");if(rc)return rc;
 rc=seed("/cache/keep.txt","CACHE_KEEP_123\n");if(rc)return rc;
 aster_handle_t f;char data[64]={0};
 rc=aster_fs_open("/assets/version.txt",19,ASTER_FS_O_RDONLY,&f);if(rc)return rc;
 int n=aster_fs_read(f,data,sizeof(data)-1);aster_fs_close(f);if(n<0)return n;
 aster_logf(ASTER_LOG_INFO,"P0 TEST version=%d save=OK cache=OK asset=%s",TEST_VERSION,data);
 aster_display_info_t info={.size=sizeof(info),.version=1};rc=aster_display_get_info(0,&info,sizeof(info));if(rc)return rc;
 aster_surface_desc_t d={.size=sizeof(d),.version=1,.width=info.app_width,.height=info.app_height,.format=ASTER_PIXEL_RGB565,.flags=ASTER_SURFACE_F_PANEL_CANVAS};
 rc=aster_surface_create(&d,sizeof(d),&surface);if(rc)return rc;
 aster_ge_init(&ge,surface,info.app_width,info.app_height,commands,8,arena,sizeof(arena));
 aster_ge_clear(&ge,TEST_VERSION==1?0x001f:0x07e0);
 aster_ge_text(&ge,20,40,20,0xffff,0,TEST_VERSION==1?"P0 TEST V1":"P0 TEST V2");
 aster_ge_present(&ge);return aster_app_set_framerate(5);
}

static volatile uint32_t spin_counter;
static void spin(void) {for(;;)spin_counter++;}
static aster_handle_t leaked_file,timer;static int effect;
int aster_app_init(void) {
 int rc=prepare();if(rc)return rc;
 rc=aster_fs_open("/assets/version.txt",19,ASTER_FS_O_RDONLY,&leaked_file);if(rc)return rc;
 rc=aster_timer_create(1000,1,77,&timer);if(rc)return rc;
 effect=aster_audio_effect_load("/assets/fx.wav",14);
 aster_logf(ASTER_LOG_INFO,"STRESS mode=%d file=%u timer=%u effect=%d",TEST_MODE,leaked_file,timer,effect);
 if(TEST_MODE==1)spin();
 if(TEST_MODE==9)return ASTER_EIO;
 return 0;
}
void aster_app_start(void){if(TEST_MODE==5)spin();}
void aster_app_event(const aster_event_t *e) {
 if(e->type!=ASTER_EVENT_FRAME)return;
 if(TEST_MODE==2)spin();
 if(TEST_MODE==8)__builtin_trap();
}
void aster_app_pause(void){if(TEST_MODE==6)spin();}
void aster_app_resume(void){if(TEST_MODE==7)spin();}
void aster_app_stop(void){if(TEST_MODE==3)spin();}
void aster_app_destroy(void){if(TEST_MODE==4)spin();if(surface)aster_surface_destroy(surface);}
