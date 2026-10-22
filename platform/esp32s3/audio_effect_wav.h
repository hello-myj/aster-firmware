/* SPDX-License-Identifier: MIT. Bounded RIFF chunk parser, no allocations. */
#ifndef ASTER_AUDIO_EFFECT_WAV_H
#define ASTER_AUDIO_EFFECT_WAV_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
typedef struct {uint32_t offset, bytes, rate, channels;} audio_effect_wav_t;
typedef int (*audio_effect_read_at_t)(void *,uint32_t,void *,size_t);
static uint16_t fx_u16(const uint8_t *p) {return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static uint32_t fx_u32(const uint8_t *p) {return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static int audio_effect_wav_parse(audio_effect_read_at_t read,void *ctx,uint64_t file_size,audio_effect_wav_t *out)
{
 uint8_t h[16];audio_effect_wav_t info={0};bool fmt=false,data=false;
 if(file_size<12 || file_size>INT32_MAX || read(ctx,0,h,12)) return -EINVAL;
 if(memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4)) return -EINVAL;
 uint64_t end=8u+(uint64_t)fx_u32(h+4),pos=12;
 if(end>file_size || end<12) return -EINVAL;
 while(pos<end) {
  if(end-pos<8 || read(ctx,(uint32_t)pos,h,8)) return -EINVAL;
  uint32_t n=fx_u32(h+4);uint64_t next=pos+8+(uint64_t)n+(n&1u);
  if(next>end) return -EINVAL;
  if(!memcmp(h,"fmt ",4)) {
   if(fmt || n<16 || read(ctx,(uint32_t)pos+8,h,16)) return -EINVAL;
   info.channels=fx_u16(h+2);info.rate=fx_u32(h+4);
   if(fx_u16(h)!=1 || (info.channels!=1 && info.channels!=2) ||
      info.rate<8000 || info.rate>48000 || fx_u16(h+14)!=16 ||
      fx_u16(h+12)!=info.channels*2 || fx_u32(h+8)!=info.rate*info.channels*2) return -EINVAL;
   fmt=true;
  } else if(!memcmp(h,"data",4)) {
   if(data || !n || n>65536) return -EINVAL;
   info.offset=(uint32_t)pos+8;info.bytes=n;data=true;
  }
  pos=next;
 }
 if(!fmt || !data || info.bytes%(info.channels*2)) return -EINVAL;
 *out=info;return 0;
}
#endif
