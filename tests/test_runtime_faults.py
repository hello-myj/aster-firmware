"""Callback fault reporting and idempotent cleanup on real runtime functions."""
import unittest
from test_waveshare import ROOT,run_c

class RuntimeFaults(unittest.TestCase):
    def test_callbacks_disarm_budget_and_preserve_actual_trap_detail(self):
        s=(ROOT/'systems/zephyr/runtime/aster_runtime.c').read_text()
        call=s[s.index('static int call_fn('):s.index('/* ---------- event loop')]
        event=s[s.index('static void deliver_event('):s.index('/* pending lifecycle')]
        header=(ROOT/'systems/zephyr/runtime/include/aster_watchdog.h').read_text()
        header='\n'.join(l for l in header.splitlines() if not l.startswith('#include'))+'\n'
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define ASTER_ETIMEDOUT -8
#define LOG_DBG(...) snprintf(message,sizeof(message),__VA_ARGS__)
#define LOG_ERR(...) snprintf(message,sizeof(message),__VA_ARGS__)
typedef struct {uint32_t type;} aster_event_t;
typedef struct {char id[64];void *exec_env,*module_inst,*f_init,*f_start,*f_event,*f_stop,*f_destroy,*f_pause;bool exit_clean,crashed,quit;} aster_app_ctx_t;
static char exception[128],message[256];static bool ok=true;static int late,arms,ends,freed,phase;
static aster_event_t ev_buf;
''' +header+r'''
void aster_watchdog_begin(const char *id,enum aster_callback_phase p){assert(*id);arms++;phase=p;}
int aster_watchdog_end(void){ends++;return late;}
static bool wasm_runtime_call_wasm(void *env,void *func,uint32_t n,uint32_t *args){(void)env;(void)func;(void)n;args[0]=42;return ok;}
static const char *wasm_runtime_get_exception(void *inst){(void)inst;return exception;}
static void wasm_runtime_clear_exception(void *inst){(void)inst;exception[0]=0;}
static uint32_t wasm_runtime_module_malloc(void *inst,size_t n,void **out){(void)inst;assert(n==sizeof(ev_buf));*out=&ev_buf;return 16;}
static void wasm_runtime_module_free(void *inst,uint32_t off){(void)inst;assert(off==16);freed++;}
''' +call+event+r'''
int main(void){
 aster_app_ctx_t c={.id="test.p0.fault",.f_init=(void*)1,.f_event=(void*)2,.f_stop=(void*)3};uint32_t ret=0;
 assert(!call_fn(&c,c.f_init,0,&ret)&&ret==42&&phase==ASTER_CB_INIT&&arms==ends);
 ok=false;strcpy(exception,"unreachable");assert(call_fn(&c,c.f_stop,0,NULL)<0&&c.crashed&&c.quit);
 assert(strstr(message,"unreachable")&&!exception[0]&&arms==ends);
 c.crashed=c.quit=false;c.exit_clean=true;strcpy(exception,"terminated by user");
 assert(call_fn(&c,c.f_init,0,NULL)<0&&!c.crashed);
 c.exit_clean=false;strcpy(exception,"out of bounds memory access");aster_event_t e={1};deliver_event(&c,&e);
 assert(c.crashed&&c.quit&&freed==1&&strstr(message,"out of bounds memory access")&&arms==ends);
 ok=true;late=1;c.crashed=c.quit=false;
 assert(call_fn(&c,c.f_stop,0,NULL)==-8&&c.crashed&&c.quit&&arms==ends);
 return 0;
}
''')
    def test_cleanup_releases_host_handles_then_each_guest_object_exactly_once(self):
        s=(ROOT/'systems/zephyr/runtime/aster_runtime.c').read_text()
        code=s[s.index('static void ctx_cleanup('):s.index('/* Load + init + start')]
        run_c(r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define CONFIG_ASTER_LVGL 1
#define IS_ENABLED(x) (x)
typedef struct {void *exec_env,*module_inst,*module,*img;} aster_app_ctx_t;
static int files=1,sprites=1,frames=1,network=1,env=1,instance=1,module=1,image=1;
static void aster_gfx_drop_all(void){sprites=0;}
static void aster_native_frame_release(aster_app_ctx_t *ctx){(void)ctx;frames=0;}
static void aster_native_release_network(aster_app_ctx_t *ctx){(void)ctx;assert(!frames);network=0;}
static void aster_native_release_files(aster_app_ctx_t *ctx){(void)ctx;assert(!frames&&!network);files=0;}
static void wasm_runtime_destroy_exec_env(void *p){assert(p&&!files&&!sprites&&env);env--;}
static void wasm_runtime_deinstantiate(void *p){assert(p&&!env&&instance);instance--;}
static void wasm_runtime_unload(void *p){assert(p&&!instance&&module);module--;}
static void aster_psram_free(void *p){assert(p&&!module&&image);image--;}
''' +code+r'''
int main(void){
 aster_app_ctx_t c={(void*)1,(void*)2,(void*)3,(void*)4};
 ctx_cleanup(&c);ctx_cleanup(&c);ctx_cleanup(NULL);
 assert(!files&&!sprites&&!frames&&!network&&!env&&!instance&&!module&&!image);
 assert(!c.exec_env&&!c.module_inst&&!c.module&&!c.img);return 0;
}
''')
