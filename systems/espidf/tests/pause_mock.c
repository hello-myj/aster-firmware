/* SPDX-License-Identifier: MIT */
#include "aster_idf_runtime.h"
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
static int failed;
static void *control(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<5000 && !aster_idf_is_running();i++)usleep(1000);
    if(aster_idf_request_pause(true))failed=1;
    usleep(200000);
    if(aster_idf_request_pause(false))failed=1;
    return NULL;
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    pthread_t thread;aster_idf_clear_stop();
    if(pthread_create(&thread,NULL,control,NULL))return 2;
    int r=aster_idf_run_installed(argv[1],"test.pause",0);pthread_join(thread,NULL);
    if(r || failed || aster_idf_is_running())return 1;
    puts("PASS pause/resume lifecycle and suppression of guest frames while paused");return 0;
}
