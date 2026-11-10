"""HTTPS initialization waits for verified wall time without weakening TLS."""
import unittest
from test_waveshare import ROOT, run_c

class SetupTime(unittest.TestCase):
    def test_waits_for_sync_and_bounds_timeout(self):
        source=(ROOT/'systems/shared/setup/aster_setup.c').read_text()
        helper=source[source.index('static int wait_https_time('):source.index('static int download(')]
        run_c(r'''
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define ASTER_TIME_FLAG_SYNCED 1
typedef struct {unsigned flags;} aster_time_info_t;
static int64_t clock_ms;static int calls,mode;
static int64_t k_uptime_get(void){return clock_ms;}
static void k_msleep(unsigned n){clock_ms+=n;}
static int aster_time_read(aster_time_info_t *out){
 calls++;out->flags=mode==1||(mode==2&&calls>=4)?1:0;return mode==3?-EAGAIN:0;
}
''' + helper + r'''
int main(void){
 assert(!wait_https_time("http://localhost")&&calls==0&&clock_ms==0);
 mode=1;assert(!wait_https_time("https://github.com")&&calls==1&&clock_ms==0);
 mode=2;calls=0;assert(!wait_https_time("https://github.com")&&calls==4&&clock_ms==300);
 mode=3;calls=0;clock_ms=0;assert(wait_https_time("https://github.com")==-ENODATA&&clock_ms==30000);
 return 0;
}
''')
