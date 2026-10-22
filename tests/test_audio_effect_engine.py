"""Exercise the production continuous-voice DSP and RIFF parser under ASan/UBSan."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class AudioEffectEngine(unittest.TestCase):
 def test_engine(self):
  with tempfile.TemporaryDirectory() as d:
   exe = str(pathlib.Path(d) / 'effects')
   subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-fsanitize=address,undefined', '-O1', '-g', '-I'+str(ROOT/'contract/include'),
    str(ROOT/'tests/audio_effect_host.c'),
    str(ROOT/'platform/esp32s3/audio_effect_engine.c'), '-o', exe], check=True)
   subprocess.run([exe], check=True, timeout=30)

 def test_guest_descriptors_are_bounded_copies(self):
  native = (ROOT/'systems/zephyr/runtime/aster_native.c').read_text()
  wrappers = native[native.index('static int32_t n_audio_effect_play_ex('):native.index('static int32_t n_audio_effect_unload(')]
  code = r'''
#include <assert.h>
#include <string.h>
#include <errno.h>
#include "audio_effect_engine.h"
#include <aster/error.h>
#define CONFIG_ASTER_AUDIO 1
#define IS_ENABLED(x) (x)
#define ARG_UNUSED(x) ((void)(x))
typedef void *wasm_exec_env_t;
static uint8_t mem[512];static int gate;
static audio_effect_voice_t voice;
static int32_t audio_out_gate(void) {return gate;}
static void *buf(wasm_exec_env_t env,uint32_t off,uint32_t n) {
 (void)env;return off<=sizeof(mem) && n<=sizeof(mem)-off?mem+off:NULL;
}
static int map_audio_err(int e) {return e==-EINVAL?ASTER_EINVAL:e==-EAGAIN?ASTER_ESTATE:e;}
static int aster_plat_audio_effect_play_ex(uint32_t h,const aster_audio_effect_play_desc_t *d) {return h==1?audio_effect_begin(&voice,d):-EINVAL;}
static int aster_plat_audio_effect_set_params(uint32_t h,const aster_audio_effect_params_t *p) {return h==1?audio_effect_params(&voice,p):-EINVAL;}
static int aster_plat_audio_effect_stop(uint32_t h,uint32_t ms) {return h==1?audio_effect_stop(&voice,ms):-EINVAL;}
''' + wrappers + r'''
int main(void) {
 static const int16_t pcm[]={1,-1,100,-100};
 voice=(audio_effect_voice_t){.pcm=pcm,.rate=16000,.frames=4,.channels=1};
 aster_audio_effect_play_desc_t d={36,1,1,0,0,0,65536,65536,0};
 memcpy(mem+17,&d,sizeof(d)); /* deliberately unaligned guest pointer */
 assert(n_audio_effect_play_ex(0,1,17,36)==0);
 memset(mem+17,0,sizeof(d));assert(voice.desc.pitch_q16==65536);
 assert(n_audio_effect_play_ex(0,1,17,35)==ASTER_EINVAL);
 assert(n_audio_effect_play_ex(0,1,UINT32_MAX,36)==ASTER_EINVAL);
 assert(n_audio_effect_play_ex(0,1,500,36)==ASTER_EINVAL);
 d.version=2;memcpy(mem+17,&d,sizeof(d));assert(n_audio_effect_play_ex(0,1,17,36)==ASTER_EINVAL);
 d.version=1;d.size=40;memcpy(mem+17,&d,sizeof(d));assert(n_audio_effect_play_ex(0,1,17,36)==ASTER_EINVAL);
 aster_audio_effect_params_t p={20,1,98304,32768,120};memcpy(mem+33,&p,sizeof(p));
 voice.phase=32768;assert(!n_audio_effect_set_params(0,1,33,20));assert(voice.phase==32768);
 assert(n_audio_effect_set_params(0,1,500,20)==ASTER_EINVAL);
 assert(n_audio_effect_stop(0,1,1001)==ASTER_EINVAL);
 assert(!n_audio_effect_stop(0,1,100));
 assert(n_audio_effect_set_params(0,1,33,20)==ASTER_ESTATE);
 assert(!n_audio_effect_stop(0,1,0));assert(!voice.playing);
 gate=ASTER_ENODEV;assert(n_audio_effect_play_ex(0,1,17,36)==ASTER_ENODEV);
 assert(n_audio_effect_set_params(0,1,33,20)==ASTER_ENODEV);
 assert(n_audio_effect_stop(0,1,0)==ASTER_ENODEV);
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   source = pathlib.Path(d)/'bridge.c'
   source.write_text(code)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
    '-fsanitize=address,undefined','-O1','-g','-I'+str(ROOT/'contract/include'),
    '-I'+str(ROOT/'platform/esp32s3'),str(source),
    str(ROOT/'platform/esp32s3/audio_effect_engine.c'),'-o',str(source.with_suffix(''))],check=True)
   subprocess.run([str(source.with_suffix(''))],check=True,timeout=10)
