/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <aster/aster.h>
#include "../platform/esp32s3/audio_stream_ring.h"
#define CONFIG_ASTER_AUDIO 1
#define IS_ENABLED(n) (n)
#define K_MSEC(n) (n)
typedef struct {uint32_t audio_stream_handle;} aster_app_ctx_t;
typedef struct {aster_app_ctx_t ctx;uint8_t memory[4096];} guest;
typedef guest *wasm_exec_env_t;
typedef guest *wasm_module_inst_t;
static int gate,closed,locked;
static aster_app_ctx_t *ctx_of(guest *g){return &g->ctx;}
static int audio_out_gate(void){return gate;}
static guest *wasm_runtime_get_module_inst(guest *g){return g;}
static bool wasm_runtime_get_app_addr_range(guest *g,uint32_t off,void *start,uint64_t *end){(void)g;(void)start;*end=4096;return off<4096;}
static void *wasm_runtime_addr_app_to_native(guest *g,uint32_t off){return g->memory+off;}
static int aster_plat_audio_stream_open(uint32_t codec){return codec==1?17:-ENOTSUP;}
static int aster_plat_audio_stream_write(uint32_t h,const void *p,size_t n){assert(h==17&&p&&n);return n>7?7:(int)n;}
static int aster_plat_audio_stream_finish(uint32_t h){assert(h==17);return 0;}
static int aster_plat_audio_stream_status(uint32_t h,aster_audio_stream_status_t *s){assert(h==17);*s=(aster_audio_stream_status_t){.size=56,.version=1,.error=-EPIPE};return 0;}
static int aster_plat_audio_stream_close(uint32_t h){assert(h==17);if(locked)return -EAGAIN;++closed;return 0;}
static void k_sleep(int n){assert(n==1);locked=0;}
#include "../systems/zephyr/runtime/aster_audio_stream_bridge.inc"
int main(void)
{
 _Static_assert(sizeof(aster_audio_stream_status_t)==56,"ABI");
 uint8_t store[31],sent[2048],out[2048];for(unsigned i=0;i<sizeof(sent);++i)sent[i]=(uint8_t)(i*7);
 struct audio_stream_ring r={.data=store,.cap=sizeof(store)};
 unsigned tx=0,rx=0;
 for(unsigned tick=0;rx<sizeof(sent)&&tick<10000;++tick){
  unsigned n=sizeof(sent)-tx;if(n>17)n=17;tx+=audio_ring_write(&r,sent+tx,n);
  if(tick%3==0)rx+=audio_ring_read(&r,out+rx,11);
  assert(r.used<=r.cap);
 }
 assert(tx==sizeof(sent)&&rx==tx&&!memcmp(sent,out,tx)&&!r.used);
 guest a={0},b={0};gate=ASTER_ENODEV;assert(n_audio_stream_open(&a,1)==ASTER_ENODEV);gate=0;
 assert(n_audio_stream_open(&a,2)==ASTER_ENOTSUP);assert(n_audio_stream_open(&a,1)==17);
 assert(n_audio_stream_open(&a,1)==ASTER_EBUSY);
 assert(n_audio_stream_write(&b,17,4,1)==ASTER_EBADF);
 assert(n_audio_stream_write(&a,17,4090,7)==ASTER_EINVAL);
 assert(n_audio_stream_write(&a,17,UINT32_MAX,7)==ASTER_EINVAL);
 assert(n_audio_stream_write(&a,17,4,2049)==ASTER_EINVAL);
 assert(n_audio_stream_write(&a,17,4,0)==ASTER_EINVAL);
 assert(n_audio_stream_write(&a,17,4,20)==7);
 assert(n_audio_stream_status(&a,17,4,55)==ASTER_EINVAL);
 assert(n_audio_stream_status(&a,17,4090,56)==ASTER_EINVAL);
 assert(!n_audio_stream_status(&a,17,5,56));aster_audio_stream_status_t s;memcpy(&s,a.memory+5,56);assert(s.error==ASTER_EPIPE);
 assert(!n_audio_stream_finish(&a,17));assert(n_audio_stream_close(&b,17)==ASTER_EBADF);
 locked=1;assert(n_audio_stream_close(&a,17)==ASTER_EAGAIN&&a.ctx.audio_stream_handle==17);
 aster_native_release_audio_stream(&a.ctx);assert(!a.ctx.audio_stream_handle&&closed==1);
 assert(n_audio_stream_finish(&a,17)==ASTER_EBADF);return 0;
}
