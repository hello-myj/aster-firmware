"""Real watchdog deadline/retained-reason code, with fake clock and reset."""
import unittest
from test_waveshare import ROOT, run_c

class CallbackWatchdog(unittest.TestCase):
    def program(self):
        src=(ROOT/'systems/zephyr/runtime/aster_watchdog.c').read_text()
        src=src[src.index('static const char *const phases'):]
        header=(ROOT/'systems/zephyr/runtime/include/aster_watchdog.h').read_text()
        header='\n'.join(l for l in header.splitlines() if not l.startswith('#include'))+'\n'
        return r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <errno.h>
#define WATCH_RETAIN
#define CONFIG_ASTER_APP_WATCHDOG 1
#define CONFIG_ASTER_APP_INIT_TIMEOUT_MS 30
#define CONFIG_ASTER_APP_EVENT_TIMEOUT_MS 20
#define CONFIG_ASTER_APP_STOP_TIMEOUT_MS 10
#define ASTER_APP_STATE_CRASHED 3
#define IS_ENABLED(x) (x)
#define K_MUTEX_DEFINE(name) int name
#define K_FOREVER 0
#define K_NO_WAIT 0
#define K_KERNEL_STACK_LEN(n) (n)
#define K_KERNEL_STACK_RESERVED 0
#define K_PRIO_PREEMPT(n) (n)
#define SYS_REBOOT_COLD 0
#define LOG_ERR(...) snprintf(message,sizeof(message),__VA_ARGS__)
#define LOG_MODULE_REGISTER(...) /* nothing */
struct k_thread {int unused;};typedef unsigned char k_thread_stack_t;
static uint32_t clock_ms;static int depth,resets,reported,created;static char message[256];
static void *stack;static jmp_buf restart;
static uint32_t k_uptime_get_32(void){return clock_ms;}
static void k_msleep(int n){clock_ms+=(uint32_t)n;}
static void k_mutex_lock(int *m,int forever){(void)m;(void)forever;assert(!depth);depth++;}
static void k_mutex_unlock(int *m){(void)m;assert(depth==1);depth--;}
static void *aster_dram_alloc(size_t n){assert(n==3072);stack=malloc(n);return stack;}
static void k_thread_create(struct k_thread *t,void *s,size_t n,void (*f)(void*,void*,void*),
 void *a,void *b,void *c,int priority,int options,int delay){
 (void)t;(void)f;(void)a;(void)b;(void)c;(void)options;(void)delay;
 assert(s==stack&&n==3072&&priority==2);created++;
}
static void k_thread_name_set(struct k_thread *t,const char *name){(void)t;assert(!strcmp(name,"aster_watch"));}
static void aster_app_mgr_set_state(const char *id,int state,uint32_t error){
 assert(!strcmp(id,"test.p0.loop")&&state==3&&error==8);reported++;
}
static void sys_reboot(int kind){assert(kind==0);resets++;longjmp(restart,1);}
''' +header+src
    def test_deadline_wrap_and_late_return_disarm_without_freeing_guest(self):
        run_c(self.program()+r'''
int main(void){
 assert(!aster_watchdog_init()&&created==1);
 clock_ms=UINT32_MAX-5;aster_watchdog_begin("test.p0.loop",ASTER_CB_EVENT);
 assert(watch.limit==20&&watch.armed);clock_ms=4;assert(!aster_watchdog_end()&&!watch.armed);
 aster_watchdog_begin("test.p0.loop",ASTER_CB_STOP);clock_ms+=10;
 assert(aster_watchdog_end()&&!watch.armed&&!resets);
 aster_watchdog_begin("test.p0.loop",ASTER_CB_INIT);assert(watch.limit==30);
 aster_watchdog_end();assert(!depth);free(stack);return 0;
}
''')
    def test_timeout_reason_survives_simulated_reboot_and_corruption_is_ignored(self):
        run_c(self.program()+r'''
int main(void){
 assert(!aster_watchdog_init());clock_ms=100;aster_watchdog_begin("test.p0.loop",ASTER_CB_DESTROY);
 if(!setjmp(restart))watch_fn(NULL,NULL,NULL);
 assert(resets==1&&previous.phase==ASTER_CB_DESTROY&&previous.elapsed==10);
 assert(previous.crc==reason_crc());free(stack);
 depth=0;memset(&watch,0,sizeof(watch));assert(!aster_watchdog_init());
 assert(reported==1&&!previous.magic&&strstr(message,"ASTER_RECOVERED"));free(stack);
 previous.magic=0x41574831u;previous.phase=ASTER_CB_EVENT;previous.id[0]^=1;
 assert(!aster_watchdog_init()&&reported==1&&!previous.magic);free(stack);return 0;
}
''')
