/* SPDX-License-Identifier: MIT */
#include <aster/aster.h>
#include "aster_gfx_engine.h"
#include "aster_printf.h"
#include "aster_libc.h"
#include "network.inc"
#ifndef N4_SCENE
#define N4_SCENE -1
#endif
static int imu_enabled;
static aster_handle_t surface;
static aster_ge_t ge;
static aster_gfx_cmd_t ring[96];static uint8_t arena[2048];
static uint32_t W,H,frames,drawerr,imuerr,audioerr,reserr,reads,fresh,stale,touches;
static uint32_t seq,imu_ms,last_log,last_frame,max_gap,max_draw,music,effects,cancels,pauses,resumes,overlay_busy;
static uint32_t phase_frames[3],phase_ms[3],last_effect,last_resource;
static uint32_t bmp,bw,bh;static int effect,oldvol;
static void report(void){
 net_report();
 aster_logf(ASTER_LOG_INFO,"CONCUR frames=%u drawerr=%u imuerr=%u audioerr=%u reserr=%u stale=%u gap=%u drawmax=%u",frames,drawerr,imuerr,audioerr,reserr,stale,max_gap,max_draw);
 aster_logf(ASTER_LOG_INFO,"CONCURIO frames=%u reads=%u fresh=%u touch=%u music=%u effects=%u cancel=%u pause=%u resume=%u overlay_busy=%u",frames,reads,fresh,touches,music,effects,cancels,pauses,resumes,overlay_busy);
 aster_logf(ASTER_LOG_INFO,"CONCURPH frames=%u p0=%u p1=%u p2=%u t0=%u t1=%u t2=%u",frames,phase_frames[0],phase_frames[1],phase_frames[2],phase_ms[0],phase_ms[1],phase_ms[2]);
}
static void start_music(void){int r=aster_audio_start_file("/assets/bed.wav",15);if(r)audioerr++;else music++;}
static void load_image(void){if(bmp){if(aster_gfx_bmp_free(bmp))reserr++;bmp=0;}
 if(aster_gfx_bmp_load("/assets/tile.bmp",16,64,64,&bmp,&bw,&bh))reserr++;
}
int aster_app_init(void){
 int r=0;imu_enabled=aster_system_has_feature(ASTER_FEATURE_ACCELEROMETER)>0&&aster_system_has_feature(ASTER_FEATURE_GYROSCOPE)>0;
 if(imu_enabled){aster_imu_info_t info={.size=sizeof(info),.version=1};r=aster_imu_get_info(&info,sizeof(info));if(r)return r;}
 net_init();
 aster_surface_desc_t d={.size=sizeof(d),.version=1,.format=ASTER_PIXEL_RGB565,.flags=ASTER_SURFACE_F_PANEL_CANVAS};
 if((r=aster_surface_create(&d,sizeof(d),&surface)))return r;
 aster_surface_buffer_t b={.size=sizeof(b),.version=1};if((r=aster_surface_acquire(surface,&b,sizeof(b))))return r;
 W=b.width;H=b.height;aster_ge_init(&ge,surface,W,H,ring,96,arena,sizeof(arena));
 oldvol=aster_audio_get_volume();aster_audio_set_volume(20);
 effect=aster_audio_effect_load("/assets/cue.wav",15);if(effect<=0)return effect?effect:ASTER_EIO;
 load_image();start_music();aster_app_set_framerate(60);
 aster_logf(ASTER_LOG_INFO,"CONCUR INIT %ux%u effect=%d bmp=%u oldvol=%d",W,H,effect,bmp,oldvol);return 0;
}
void aster_app_pause(void){pauses++;last_frame=0;}
void aster_app_resume(void){resumes++;last_frame=0;imu_ms=aster_time_now_ms();aster_audio_set_volume(20);}
void aster_app_event(const aster_event_t *e){
 if(e->type==ASTER_EVENT_TOUCH){touches++;return;}
 if(e->type!=ASTER_EVENT_FRAME)return;
 uint32_t now=aster_time_now_ms();if(last_frame&&now-last_frame>max_gap)max_gap=now-last_frame;last_frame=now;
 net_tick(now);
 if(imu_enabled){aster_imu_sample_t sample={.size=sizeof(sample),.version=1};reads++;
 if(aster_imu_read(&sample,sizeof(sample)))imuerr++;
 else if((sample.flags&3u)!=3u)imuerr++;
 else if(!fresh||sample.sequence!=seq){fresh++;seq=sample.sequence;imu_ms=now;}
 else if(now-imu_ms>1000u){stale++;imu_ms=now;} }
 aster_audio_state_t as={.size=sizeof(as),.version=2};
 if(aster_audio_get_state(&as,sizeof(as)))audioerr++;
 else if(as.state==ASTER_AUDIO_IDLE)start_music();
 if(now-last_effect>=700u){last_effect=now;if(aster_audio_effect_play(effect))audioerr++;else effects++;}
 if(now-last_resource>=60000u){last_resource=now;load_image();}
 unsigned phase=N4_SCENE<0?(now/20000u)%3u:(unsigned)N4_SCENE;int rc=0;
 if(phase==2){rc=aster_ge_begin_frame(&ge);
  /* The overlay owns the glass: public frame BEGIN intentionally reports BUSY.
   * Keep servicing network/audio and report this separately from draw faults. */
  if(rc==ASTER_EBUSY){overlay_busy++;last_frame=0;if(now-last_log>=5000u){last_log=now;report();}return;}
  if(rc){drawerr++;return;}}
 /* A panel canvas alternates backing buffers and never retains partial
  * frames. Repaint the background even in the low-geometry phase. */
 aster_ge_clear(&ge,0x0841);
 if(phase==0){for(unsigned i=0;i<12;i++)aster_ge_rect(&ge,(i%4u)*(W-8u)/3u,32+(i/4u)*(H-64u)/2u,8,8,(uint16_t)(0xf800u+(frames*97u+i*16u)%0x7ffu));}
 else {aster_ge_grad(&ge,0,0,W,H,0x0841,0x001f,0);
 for(unsigned i=0;i<24;i++)aster_ge_rect(&ge,(frames*3u+i*17u)%(W-24u),32+(frames+i*13u)%(H-64u),24,12,(uint16_t)(0x4208u+i*199u));
 for(unsigned i=0;i<5;i++)aster_ge_text(&ge,4,64+i*32,16,0xffff,ASTER_GFX_NOBG,"图形 IMU 音频 Stress");}
 if(bmp)aster_ge_bmp(&ge,bmp,bw,bh,W/2-16,H-48,phase==1?384:256,phase==2?160:255);
 char line[64];aster_snprintf(line,sizeof(line),"F%u I%u T%u P%u",frames,fresh,touches,phase);
 aster_ge_rect(&ge,0,0,W,24,0x0841);aster_ge_text(&ge,2,18,16,0xffff,ASTER_GFX_NOBG,line);
 if(phase==2&&frames%17u==0){rc=aster_ge_cancel_frame(&ge);cancels++;}
 else rc=phase==0?aster_ge_present_commands(&ge):aster_ge_present(&ge);
 if(rc<0)drawerr++;
 uint32_t elapsed=aster_time_now_ms()-now;phase_frames[phase]++;phase_ms[phase]+=elapsed;if(elapsed>max_draw)max_draw=elapsed;
 frames++;if(now-last_log>=5000u){last_log=now;report();}
}
void aster_app_stop(void){net_stop();report();aster_audio_stop();if(effect>0)aster_audio_effect_unload(effect);effect=0;if(oldvol>=0)aster_audio_set_volume(oldvol);}
void aster_app_destroy(void){if(bmp)aster_gfx_bmp_free(bmp);bmp=0;if(surface)aster_surface_destroy(surface);surface=0;}
