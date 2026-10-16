/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../platform/esp32s3/audio_rate.h"
int main(void)
{
 const uint32_t rates[]={8000,11025,12000,16000,22050,24000,32000,44100,48000};
 for(unsigned k=0;k<sizeof(rates)/sizeof(rates[0]);++k){
  uint32_t rate=rates[k],fraction=0;size_t cursor=0;
  assert(audio_mp3_rate(rate)==rate&&audio_rate_block(rate)<=1536);
  assert(audio_rate_block(rate)%32==0);
  /* Exactly one second of output consumes one second of native cues. */
  for(uint32_t i=0;i<rate;++i)audio_rate_advance(&cursor,&fraction,16000,rate);
  assert(cursor==16000&&fraction==0);
 }
 assert(!audio_mp3_rate(0)&&!audio_mp3_rate(96000));
 int16_t pcm[]={-32768,32767,32767,-32768};
 assert(audio_rate_sample(pcm,2,2,0,0,0,48000)==-32768);
 assert(audio_rate_sample(pcm,2,2,0,0,24000,48000)==-1);
 assert(audio_rate_sample(pcm,2,2,1,0,24000,48000)==0);
 assert(audio_rate_sample(pcm,2,2,0,1,40000,48000)==32767);
 assert(audio_rate_sample(pcm,2,2,0,2,0,48000)==0);
 /* Chunking at DMA boundaries must not change the rational cursor. */
 size_t frame=0;uint32_t f=0;
 for(unsigned b=0;b<125;++b)
  for(unsigned i=0;i<384;++i)audio_rate_advance(&frame,&f,16000,48000);
 assert(frame==16000&&f==0);
 puts("audio rate interpolation and exact duration OK");return 0;
}
