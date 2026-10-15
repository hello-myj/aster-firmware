/* SPDX-License-Identifier: MIT. Generate sample.inc with audio_stream_check.py. */
#include <aster_imports.h>
static const unsigned char song[]={
#include "sample.inc"
};
static int h,stage,failed,paused,paused_once,full;
static unsigned off,part,frames,done;
static uint64_t at;
static uint32_t frozen;
static void check(int yes,const char *where,int rc){if(!yes&&!failed){failed=1;aster_logf(ASTER_LOG_ERROR,"STREAMCHECK FAIL %s rc=%d stage=%d",where,rc,stage);}}
int aster_app_init(void){return 0;}
void aster_app_event(const aster_event_t *e){
 if(e->type!=ASTER_EVENT_FRAME||failed||done)return;
 if(++frames>1800){check(0,"timeout",0);return;}
 if(!h){int rc=aster_audio_stream_open(ASTER_AUDIO_STREAM_MP3);if(rc==ASTER_EAGAIN||rc==ASTER_EBUSY)return;check(rc>0,"open",rc);if(failed)return;h=rc;off=0;at=aster_time_now_ms();}
 aster_audio_stream_status_t s;int rc=aster_audio_stream_status(h,&s,sizeof(s));if(rc==ASTER_EAGAIN)return;check(!rc,"status",rc);if(failed)return;
 if(stage==0){
  if(!off){check(aster_audio_stream_write(h,song,0)==ASTER_EINVAL,"zero",0);check(aster_audio_stream_write(h,song,2049)==ASTER_EINVAL,"oversize",0);}
  if(!paused_once&&s.played_ms>=600){rc=aster_audio_pause();if(!rc){paused=paused_once=1;frozen=s.played_ms;at=aster_time_now_ms();}}
  if(paused&&aster_time_now_ms()-at>1200){check(s.played_ms<=frozen+100,"pause cursor",s.played_ms);rc=aster_audio_resume();if(!rc)paused=0;}
  if(off<sizeof(song)){
   unsigned n=sizeof(song)-off;if(n>2048)n=2048;
   rc=aster_audio_stream_write(h,song+off,n);
   if(rc==ASTER_EAGAIN){full=1;return;}check(rc>0&&rc<=(int)n,"write",rc);if(rc>0){if(rc<(int)n)++part;off+=rc;}
  }else{rc=aster_audio_stream_finish(h);if(rc==ASTER_EAGAIN)return;check(!rc,"finish",rc);check(aster_audio_stream_write(h,song,1)==ASTER_EPIPE,"post EOF",0);}
  if(s.state==ASTER_AUDIO_STREAM_FAILED){check(0,"decoder",s.error);return;}
  if(s.state!=ASTER_AUDIO_STREAM_DONE)return;
  check(s.received==sizeof(song)&&s.played_ms>1000&&paused_once&&full,"decoded totals",s.played_ms);
  aster_logf(ASTER_LOG_INFO,"STREAMCHECK decoded bytes=%u played=%u partial=%u full=%d",(unsigned)s.received,s.played_ms,part,full);
 }else if(stage==1){
  if(!off){rc=aster_audio_stream_finish(h);if(rc==ASTER_EAGAIN)return;check(!rc,"empty finish",rc);off=1;}
  if(s.state<ASTER_AUDIO_STREAM_DONE)return;check(s.state==ASTER_AUDIO_STREAM_FAILED&&s.error==ASTER_EPROTO,"empty invalid",s.error);
 }else if(stage==2){
  static const unsigned char invalid[]={ 'I','D','3',5,0,0,0,0,0,0};
  if(!off){rc=aster_audio_stream_write(h,invalid,sizeof(invalid));if(rc==ASTER_EAGAIN)return;check(rc==sizeof(invalid),"bad tag write",rc);off=1;}
  rc=aster_audio_stream_finish(h);if(rc==ASTER_EAGAIN)return;check(!rc,"bad tag finish",rc);
  if(s.state<ASTER_AUDIO_STREAM_DONE)return;check(s.state==ASTER_AUDIO_STREAM_FAILED&&s.error==ASTER_EPROTO,"bad tag status",s.error);
 }else if(stage==3){
  aster_logf(ASTER_LOG_INFO,"STREAMCHECK PASS: partial/backpressure pause EOF malformed close; leaving final open handle for exit cleanup");done=1;return;
 }
 rc=aster_audio_stream_close(h);if(rc==ASTER_EAGAIN)return;check(!rc,"close",rc);check(aster_audio_stream_finish(h)==ASTER_EBADF,"stale",0);h=0;++stage;
}
