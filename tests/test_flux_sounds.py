"""Exercise actual nonblocking UI voice and generated PCM assets."""
import importlib.util
import unittest
from test_waveshare import ROOT, run_c

class Sounds(unittest.TestCase):
    def test_assets_match_sources(self):
        path=ROOT/'systems/zephyr/ui/launcher/flux-sounds/bake.py'
        spec=importlib.util.spec_from_file_location('bake',path)
        bake=importlib.util.module_from_spec(spec);spec.loader.exec_module(bake)
        self.assertEqual(bake.render(),(path.parent.parent/'flux_sounds.inc').read_text())
        for name in bake.NAMES:
            samples=bake.samples(path.parent/(name+'.wav'))
            self.assertGreater(len(samples),500)
            self.assertLessEqual(len(samples),4480)
            self.assertGreater(max(samples),12000)
            self.assertLessEqual(max(map(abs,samples)),26213)

    def test_nonblocking_priority_mute_and_app_session(self):
        s=(ROOT/'platform/esp32s3/audio.c').read_text()
        code=s[s.index('int aster_plat_audio_ui_play('):s.index('/* On launcher handoff')]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#define CONFIG_ASTER_AUDIO_BITS 16
#define ASTER_AUDIO_IDLE 0
#define ASTER_AUDIO_PLAYING 1
#define ASTER_AUDIO_PAUSED 2
#define K_NO_WAIT 0
#define K_FOREVER -1
static int sess_mu,effect_mu,wakes,locked,g_vol=70,wake_error,clears;
static int k_mutex_lock(int *mu,int wait) {(void)mu;assert(wait==0 || wait==-1);return locked;}
static void k_mutex_unlock(int *mu) {(void)mu;}
static struct {bool effect_closing,stop,effect_only;size_t size,off;int err,state;} sess;
static struct {const int16_t *pcm;size_t frames,off;uint32_t fraction;unsigned gain,priority;} ui_voice;
static int wake_player(void) {wakes++;return wake_error;}
static void sess_clear_locked(void) {clears++;sess.state=ASTER_AUDIO_IDLE;sess.effect_only=false;}
''' + code + r'''
int main(void) {
 static const int16_t a[]={100,200}, b[]={300,400};
 assert(!aster_plat_audio_ui_play(a,2,1000,0));
 assert(wakes==1 && sess.effect_only && sess.state==ASTER_AUDIO_PLAYING);
 assert(!aster_plat_audio_ui_play(b,2,700,1));
 assert(aster_plat_audio_ui_play(a,2,1000,0)==-EAGAIN && ui_voice.pcm==b);
 aster_plat_audio_ui_stop();assert(!ui_voice.pcm && sess.state==ASTER_AUDIO_PLAYING);
 sess.effect_only=false; /* APP background stays running and gains a UI overlay */
 assert(!aster_plat_audio_ui_play(a,2,350,0) && !sess.effect_only && wakes==1);
 locked=1;assert(aster_plat_audio_ui_play(b,2,1000,1)==-EAGAIN);locked=0;
 sess.effect_closing=true;assert(aster_plat_audio_ui_play(b,2,1000,1)==-EAGAIN);
 sess.effect_closing=false;sess.state=ASTER_AUDIO_PAUSED;
 assert(aster_plat_audio_ui_play(b,2,1000,1)==-EAGAIN);
 g_vol=0;assert(aster_plat_audio_ui_play(b,2,1000,1)==-EAGAIN);
 assert(aster_plat_audio_ui_play(b,2,1001,1)==-EINVAL);
 g_vol=70;sess.state=ASTER_AUDIO_IDLE;wake_error=-ENOMEM;
 assert(aster_plat_audio_ui_play(b,2,1000,1)==-ENOMEM);
 assert(clears==1 && sess.state==ASTER_AUDIO_IDLE && !sess.effect_only);
 return 0;
}
''')

    def test_tick_cadence_gain_and_quiet(self):
        s=(ROOT/'systems/zephyr/ui/launcher/flux.c').read_text()
        code=s[s.index('static void sound_play('):s.index('static lv_obj_t *box(')]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MAX(a,b) ((a)>(b)?(a):(b))
enum { S_TICK,S_TAP,S_OPEN,S_CLOSE,S_TUNE };
static const int16_t sound_tick[]={1},sound_tap[]={2},sound_open[]={3},sound_close[]={4},sound_tune[]={5};
static uint64_t now=100,sound_tick_ms;
static bool quiet;
static int volume_pending=-1,volume=70,plays,stops;
static unsigned last_gain,last_priority;
static uint64_t k_uptime_get(void) {return now;}
static int aster_plat_audio_get_volume(void) {return volume;}
static void aster_plat_audio_ui_stop(void) {stops++;}
static int aster_plat_audio_ui_play(const int16_t *pcm,size_t frames,unsigned gain,unsigned priority) {
 assert(pcm && frames==1);plays++;last_gain=gain;last_priority=priority;return 0;
}
''' + code + r'''
int main(void) {
 sound_play(S_TICK,0);assert(plays==1 && last_gain==1000 && last_priority==0);
 now=194;sound_play(S_TICK,0);assert(plays==1);
 now=195;sound_play(S_TICK,500);assert(plays==2 && last_gain==800);
 now=350;sound_play(S_TICK,1000);assert(plays==2);
 now=355;sound_play(S_TICK,-1000);assert(plays==3 && last_gain==600);
 now=515;sound_play(S_TICK,1700);assert(plays==4 && last_gain==350);
 sound_play(S_OPEN,0);assert(plays==5 && last_priority==1);
 quiet=true;sound_play(S_TAP,0);assert(plays==5 && stops==1);
 quiet=false;volume_pending=0;sound_play(S_TAP,0);assert(plays==5 && stops==2);
 volume_pending=-1;volume=0;sound_play(S_TAP,0);assert(plays==5 && stops==3);
 return 0;
}
''')

    def test_actual_mix_duplicates_mono_and_saturates(self):
        s=(ROOT/'platform/esp32s3/audio.c').read_text()
        code=s[s.index('static int i2s_submit('):s.index('static int i2s_recover(')]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define AUDIO_BLOCK 16
#define ASTER_PLAT_AUDIO_FRAME 4
#define CONFIG_ASTER_AUDIO_CHANNELS 2
#define CONFIG_ASTER_AUDIO_RATE 16000
#include "esp32s3/audio_rate.h"
static size_t output_block=AUDIO_BLOCK;
static uint32_t output_rate=CONFIG_ASTER_AUDIO_RATE;
#define EFFECT_SLOTS 4
#define K_FOREVER -1
#define MIN(a,b) ((a)<(b)?(a):(b))
#define CLAMP(v,lo,hi) ((v)<(lo)?(lo):(v)>(hi)?(hi):(v))
static int effect_mu,i2s_dev,tx_slab,i2s_queued;
static void k_mutex_lock(int *a,int b) {(void)a;(void)b;}
static void k_mutex_unlock(int *a) {(void)a;}
static int i2s_write(int dev,void *blk,int bytes) {(void)dev;(void)blk;assert(bytes==AUDIO_BLOCK);return 0;}
static void k_mem_slab_free(int *slab,void *blk) {(void)slab;(void)blk;}
static int i2s_kick(bool *started,bool force) {(void)force;*started=true;return 0;}
static struct {uint8_t *pcm;size_t size,off;uint32_t fraction;bool playing;} effects[EFFECT_SLOTS];
static struct {const int16_t *pcm;size_t frames,off;uint32_t fraction;unsigned gain;} ui_voice;
''' + code + r'''
int main(void) {
 static int16_t mono[]={2000,-2000,30000}, app[]={30000,30000,0,0,30000,30000,123,456};
 ui_voice.pcm=mono;ui_voice.frames=3;ui_voice.gain=500;
 bool started=false;assert(!i2s_submit(app,&started));
 assert(started && app[0]==31000 && app[1]==31000);
 assert(app[2]==-1000 && app[3]==-1000);
 assert(app[4]==32767 && app[5]==32767);
 assert(app[6]==123 && app[7]==456 && !ui_voice.pcm && ui_voice.off==3);
 return 0;
}
''')
