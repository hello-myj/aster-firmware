from test_waveshare import ROOT, run_c
import unittest


class StoreQueue(unittest.TestCase):
    def test_install_runs_only_after_cleanup_and_receipt_is_updated(self):
        source = (ROOT / 'systems/zephyr/services/aster_app_mgr.c').read_text()
        source = source[source.index('static struct {char owner[64],package[256],resources[256];} pending_install;'):]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define ASTER_EINVAL -1
#define ASTER_EBUSY -5
#define ASTER_EIO -7
#define ASTER_ESTATE -12
#define ASTER_EAGAIN -15
static int depth,installed,scans,receipt,live,fail_receipt;
static void aster_app_mgr_files_lock(void){depth++;}
static void aster_app_mgr_files_unlock(void){assert(depth);depth--;}
static int aster_kv_set(const char *owner,const char *key,const void *p,size_t n){assert(depth && !strcmp(owner,"aster.store") && !strcmp(key,"store.install.result") && n==4);memcpy(&receipt,p,4);return fail_receipt;}
static int aster_pkg_install_bundle_file(const char *p,const char *r,const char *caller){assert(depth && !live && !strcmp(p,"package") && !strcmp(r,"resources") && !strcmp(caller,"aster.store"));installed++;return 0;}
static int aster_app_mgr_scan(void){assert(depth && !live);scans++;return 1;}
''' + source + r'''
int main(void) {
 live=1;assert(!aster_app_mgr_queue_install("aster.store","package","resources"));
 assert(receipt==ASTER_EAGAIN && installed==0);
 assert(aster_app_mgr_queue_install("aster.store","package","resources")==ASTER_EBUSY);
 assert(!aster_app_mgr_finish_install("other.app",true));live=0;
 assert(aster_app_mgr_finish_install("aster.store",true));assert(installed==1 && scans==1 && receipt==0);
 assert(!aster_app_mgr_finish_install("aster.store",true));
 assert(!aster_app_mgr_queue_install("aster.store","package","resources"));
 assert(aster_app_mgr_finish_install("aster.store",false));assert(installed==1 && receipt==ASTER_ESTATE);
 fail_receipt=1;assert(aster_app_mgr_queue_install("aster.store","package","resources")==ASTER_EIO);
 assert(!aster_app_mgr_finish_install("aster.store",true));assert(!depth);return 0;
}
''')
