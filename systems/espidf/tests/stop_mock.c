/* SPDX-License-Identifier: MIT */
#include "aster_idf_runtime.h"
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>
#include <aster/error.h>
static int automatic;
static atomic_bool seen;
static void *stop(void *unused)
{
    (void)unused;
    for(int i=0;i<5000;i++) {
        if(aster_idf_is_running()) {atomic_store(&seen,true);break;}
        usleep(1000);
    }
    if(!automatic){usleep(20000);aster_idf_request_stop();return NULL;}
    for(int i=0;i<5000 && aster_idf_is_running();i++){aster_idf_check_watchdog();usleep(1000);}
    return NULL;
}
int main(int argc,char **argv)
{
    if(argc<2 || argc>3)return 2;
    automatic=argc==3;
    pthread_t thread;aster_idf_clear_stop();
    if(pthread_create(&thread,NULL,stop,NULL))return 2;
    int result=aster_idf_run_installed(argv[1],"test.stop",0);
    pthread_join(thread,NULL);
    if(result!=(automatic?ASTER_ETIMEDOUT:0) || !atomic_load(&seen) || aster_idf_is_running())return 1;
    puts(automatic?"PASS watchdog terminates a non-returning callback with ETIMEDOUT":
        "PASS stop interrupts a non-returning WASM callback and releases the instance");return 0;
}
