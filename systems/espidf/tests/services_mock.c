/* SPDX-License-Identifier: MIT */
#include "aster_idf_runtime.h"
#include "aster_storage.h"
#include "aster_perms.h"
#include "aster_app_mgr.h"
#include <string.h>
#include <stdio.h>
int main(int argc,char **argv)
{
    if(argc!=4 || aster_storage_init())return 2;
    if(aster_app_mgr_init() || aster_app_mgr_install(argv[2]) || aster_app_mgr_install(argv[3]))return 3;
    if(aster_app_mgr_count()!=2 || aster_app_mgr_scan()!=2)return 5;
    aster_idf_clear_stop();
    if(aster_idf_run_installed(argv[1],"test.denied",0))return 4;
    for(unsigned i=0;i<12;i++) {
        aster_idf_clear_stop();
        aster_app_mgr_set_state("test.services",ASTER_APP_STATE_RUNNING,0);
        int r=aster_idf_run_installed(argv[1],"test.services",ASTER_PERM_FS|ASTER_PERM_KV|ASTER_PERM_APPMGR);
        aster_app_mgr_set_state("test.services",ASTER_APP_STATE_STOPPED,0);
        if(r){fprintf(stderr,"services run %u failed: %d\n",i,r);return 1;}
        char next[64];if(!aster_idf_take_handoff(next) || strcmp(next,"test.next"))return 6;
    }
    if(aster_app_mgr_install(argv[2]) || aster_app_mgr_scan()!=2)return 7;
    aster_app_rec_t rec;if(!aster_app_mgr_find_snapshot("test.services",&rec) || rec.code_slot!=1)return 8;
    struct fs_dirent st;if(fs_stat("/SD:/aster/apps/test.services/data/value",&st) || st.size!=6)return 9;
    puts("PASS WASM permissions/FS/KV/timers, app handoff, portable package install/recovery/upgrade and cleanup");
    return 0;
}
