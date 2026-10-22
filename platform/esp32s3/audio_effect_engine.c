/* SPDX-License-Identifier: MIT */
#include "audio_effect_engine.h"
#include <errno.h>

static bool parameters(uint32_t pitch,uint32_t gain,uint32_t ms)
{return pitch>=32768 && pitch<=131072 && gain<=65536 && ms<=1000;}

int audio_effect_begin(audio_effect_voice_t *v,const aster_audio_effect_play_desc_t *d)
{
 if(!v || !v->pcm || !v->frames || !v->rate ||
    (v->channels!=1 && v->channels!=2) || !d || d->size!=sizeof(*d) ||
    d->version!=1 || d->flags&~ASTER_AUDIO_EFFECT_F_LOOP ||
    !parameters(d->pitch_q16,d->gain_q16,d->fade_in_ms)) return -EINVAL;
 uint32_t end=d->loop_end_frame?d->loop_end_frame:v->frames;
 if(d->flags&ASTER_AUDIO_EFFECT_F_LOOP) {
  if(d->loop_start_frame>=end || end>v->frames ||
     d->crossfade_frames>(end-d->loop_start_frame)/2) return -EINVAL;
 } else if(d->loop_start_frame || d->loop_end_frame || d->crossfade_frames) return -EINVAL;
 v->desc=*d;v->desc.loop_end_frame=end;
 v->phase=0;v->cached_rate=0;v->cached_pitch=0;
 v->step_error=0;v->ramp_rate=0;
 v->pitch=(int32_t)d->pitch_q16;v->gain=d->fade_in_ms?0:(int32_t)d->gain_q16;
 v->pitch_from=v->pitch;v->gain_from=v->gain;
 v->pitch_target=d->pitch_q16;v->gain_target=d->gain_q16;
 v->ramp_ms=d->fade_in_ms;v->ramp_count=0;v->ramp_total=0;
 v->playing=true;v->stopping=false;
 return 0;
}

int audio_effect_params(audio_effect_voice_t *v,const aster_audio_effect_params_t *p)
{
 if(!v || !v->pcm || !p || p->size!=sizeof(*p) || p->version!=1 ||
    !parameters(p->pitch_q16,p->gain_q16,p->ramp_ms)) return -EINVAL;
 if(v->stopping) return -EAGAIN;
 v->pitch_from=v->pitch;v->gain_from=v->gain;
 v->pitch_target=p->pitch_q16;v->gain_target=p->gain_q16;
 v->ramp_ms=p->ramp_ms;v->ramp_count=0;v->ramp_total=0;
 v->ramp_rate=0;
 if(!p->ramp_ms) {v->pitch=(int32_t)p->pitch_q16;v->gain=(int32_t)p->gain_q16;}
 return 0;
}

int audio_effect_stop(audio_effect_voice_t *v,uint32_t ms)
{
 if(!v || !v->pcm || ms>1000) return -EINVAL;
 if(!v->playing) return 0;
 if(!ms) {v->playing=false;v->stopping=false;return 0;}
 v->pitch_from=v->pitch;v->gain_from=v->gain;
 v->pitch_target=(uint32_t)v->pitch;v->gain_target=0;
 v->ramp_ms=ms;v->ramp_count=0;v->ramp_total=0;v->stopping=true;
 v->ramp_rate=0;
 return 0;
}

static int32_t at(const audio_effect_voice_t *v,uint64_t phase,unsigned channel,bool wrap)
{
 uint32_t f=(uint32_t)(phase>>16),next=f+1;
 uint32_t end=wrap?v->desc.loop_end_frame:v->frames;
 if(f>=end) return 0;
 if(next>=end) next=wrap?v->desc.loop_start_frame:f;
 unsigned c=v->channels==1?0:channel;
 int32_t a=v->pcm[(size_t)f*v->channels+c],b=v->pcm[(size_t)next*v->channels+c];
 return a+(int32_t)((int64_t)(b-a)*(phase&65535)/65536);
}

void audio_effect_sample(audio_effect_voice_t *v,uint32_t rate,int32_t *l,int32_t *r)
{
 if(!v->playing || !rate) return;
 if(v->ramp_ms) {
  if(v->ramp_rate!=rate) {
   if(v->ramp_rate) v->ramp_count=(uint32_t)((uint64_t)v->ramp_count*rate/v->ramp_rate);
   v->ramp_total=(uint32_t)(((uint64_t)rate*v->ramp_ms+999)/1000);
   v->ramp_rate=rate;
  }
  if(++v->ramp_count>v->ramp_total) v->ramp_count=v->ramp_total;
  v->pitch=v->pitch_from+(int32_t)(((int64_t)v->pitch_target-v->pitch_from)*v->ramp_count/v->ramp_total);
  v->gain=v->gain_from+(int32_t)(((int64_t)v->gain_target-v->gain_from)*v->ramp_count/v->ramp_total);
  if(v->ramp_count==v->ramp_total) {
   v->ramp_ms=0;
   if(v->stopping) {v->playing=false;v->stopping=false;return;}
  }
 }
 bool loop=(v->desc.flags&ASTER_AUDIO_EFFECT_F_LOOP)!=0;
 uint64_t end=(uint64_t)v->desc.loop_end_frame<<16;
 uint32_t fade=v->desc.crossfade_frames;
 int32_t left=at(v,v->phase,0,loop),right=at(v,v->phase,1,loop);
 if(loop && fade && v->phase>=end-((uint64_t)fade<<16)) {
  uint64_t delta=v->phase-(end-((uint64_t)fade<<16));
  uint32_t weight=(uint32_t)(delta/fade);
  uint64_t head=((uint64_t)v->desc.loop_start_frame<<16)+delta;
  left+=(int32_t)((int64_t)(at(v,head,0,false)-left)*weight/65536);
  right+=(int32_t)((int64_t)(at(v,head,1,false)-right)*weight/65536);
 }
 *l+=(int32_t)((int64_t)left*v->gain/65536);
 *r+=(int32_t)((int64_t)right*v->gain/65536);
 if(v->cached_rate!=rate || v->cached_pitch!=(uint32_t)v->pitch) {
  if(v->cached_rate && v->cached_rate!=rate)
   v->step_error=(uint32_t)((uint64_t)v->step_error*rate/v->cached_rate);
  v->cached_rate=rate;v->cached_pitch=(uint32_t)v->pitch;
  v->cached_step=(uint64_t)v->rate*(uint32_t)v->pitch/rate;
  v->step_remainder=(uint32_t)((uint64_t)v->rate*(uint32_t)v->pitch%rate);
 }
 v->step_error+=v->step_remainder;
 v->phase+=v->cached_step+v->step_error/rate;
 v->step_error%=rate;
 if(v->phase>=end) {
  if(loop) {
   uint64_t start=(uint64_t)(v->desc.loop_start_frame+fade)<<16;
   v->phase=start+(v->phase-end)%(end-start);
  } else v->playing=false;
 }
}
