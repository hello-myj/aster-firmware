/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../platform/esp32s3/audio_effect_engine.h"
#include "../platform/esp32s3/audio_effect_wav.h"
static int16_t pcm[32768];
static aster_audio_effect_play_desc_t desc(void) {
 return (aster_audio_effect_play_desc_t){.size=36,.version=1,
  .pitch_q16=65536,.gain_q16=65536,.flags=ASTER_AUDIO_EFFECT_F_LOOP};
}
static void sample(audio_effect_voice_t *v,unsigned rate) {
 int32_t l=0,r=0;audio_effect_sample(v,rate,&l,&r);
 assert(l==r);assert(l>=-32768 && l<=32767);
}
static uint8_t wav[128];static size_t wav_len;
static int read_at(void *ctx,uint32_t off,void *dst,size_t n) {
 (void)ctx;if(off>wav_len || n>wav_len-off)return -1;memcpy(dst,wav+off,n);return 0;
}
static void u32(unsigned off,uint32_t n) {for(unsigned i=0;i<4;i++)wav[off+i]=(uint8_t)(n>>(8*i));}
static void make_wav(void) {
 memset(wav,0,sizeof(wav));memcpy(wav,"RIFF",4);memcpy(wav+8,"WAVEJUNK",8);
 u32(16,3); /* odd padding, fmt starts at 24 */
 memcpy(wav+24,"fmt ",4);u32(28,16);wav[32]=1;wav[34]=1;u32(36,16000);
 u32(40,32000);wav[44]=2;wav[46]=16;
 memcpy(wav+48,"data",4);u32(52,8);wav_len=64;u32(4,(uint32_t)wav_len-8);
}
int main(void) {
 _Static_assert(sizeof(aster_audio_effect_play_desc_t)==36,"fixed ABI");
 _Static_assert(sizeof(aster_audio_effect_params_t)==20,"fixed ABI");
 for(unsigned i=0;i<32768;i++)pcm[i]=(int16_t)(i*17);
 audio_effect_voice_t v={.pcm=pcm,.frames=16000,.size=32000,.rate=16000,.channels=1};
 aster_audio_effect_play_desc_t d=desc();
 const unsigned rates[]={8000,11025,16000,22050,44100,48000};
 for(unsigned k=0;k<sizeof(rates)/sizeof(*rates);k++) {
  assert(!audio_effect_begin(&v,&d));
  /* Ten minutes with no APP callbacks, exact rational wrap, no drift. */
  for(unsigned i=0;i<rates[k]*600;i++)sample(&v,rates[k]);
  assert(v.playing && !v.phase && !v.step_error);
 }
 d.loop_start_frame=3;d.loop_end_frame=11;d.crossfade_frames=2;
 assert(!audio_effect_begin(&v,&d));
 for(unsigned i=0;i<100000;i++) {sample(&v,48000);assert(v.phase<((uint64_t)11<<16));}
 d.crossfade_frames=5;assert(audio_effect_begin(&v,&d)==-EINVAL);
 d=desc();d.flags=0;assert(!audio_effect_begin(&v,&d));
 for(unsigned i=0;i<48000;i++)sample(&v,48000);
 assert(!v.playing);
 d=desc();assert(!audio_effect_begin(&v,&d));sample(&v,16000);
 uint64_t phase=v.phase;
 aster_audio_effect_params_t p={20,1,131072,0,0};assert(!audio_effect_params(&v,&p));
 assert(v.phase==phase);sample(&v,16000);assert(v.phase==phase+131072);
 assert(!audio_effect_stop(&v,10));
 for(unsigned i=0;i<159;i++)sample(&v,16000);
 assert(v.playing);sample(&v,16000);assert(!v.playing);assert(!audio_effect_stop(&v,0));
 d=desc();d.fade_in_ms=10;assert(!audio_effect_begin(&v,&d));
 for(unsigned i=0;i<80;i++)sample(&v,16000);
 assert(v.gain==32768);
 for(unsigned i=0;i<240;i++)sample(&v,48000);
 assert(v.gain==65536 && !v.ramp_ms);
 /* Equal interpolation for both channels, shared cursor. */
 int16_t stereo[]={-10000,10000,10000,-10000};v.pcm=stereo;v.channels=2;v.frames=2;v.rate=16000;
 d=desc();assert(!audio_effect_begin(&v,&d));v.phase=32768;
 int32_t l=0,r=0;audio_effect_sample(&v,16000,&l,&r);assert(!l&&!r);
 /* Exact crossfade overlap: wrap skips the head already mixed into tail. */
 int16_t overlap[]={0,2,4,6,8,10,12,14};
 v.pcm=overlap;v.channels=1;v.frames=8;v.rate=16000;
 d=desc();d.loop_start_frame=2;d.loop_end_frame=8;d.crossfade_frames=2;
 assert(!audio_effect_begin(&v,&d));
 const int expected[]={0,2,4,6,8,10,12,10,8,10,12,10};
 for(unsigned i=0;i<sizeof(expected)/sizeof(*expected);i++) {
  int32_t a=0,b=0;audio_effect_sample(&v,16000,&a,&b);assert(a==expected[i] && b==a);
 }
 audio_effect_wav_t out;make_wav();assert(!audio_effect_wav_parse(read_at,0,wav_len,&out));
 assert(out.offset==56 && out.bytes==8 && out.channels==1 && out.rate==16000);
 for(unsigned n=0;n<64;n++)assert(audio_effect_wav_parse(read_at,0,n,&out));
 wav[44]=4;assert(audio_effect_wav_parse(read_at,0,wav_len,&out));
 make_wav();u32(52,65537);assert(audio_effect_wav_parse(read_at,0,wav_len,&out));
 make_wav();u32(16,UINT32_MAX);assert(audio_effect_wav_parse(read_at,0,wav_len,&out));
 puts("continuous voices: rate/pitch/loop/ramp/stereo/RIFF bounds OK");
 return 0;
}
