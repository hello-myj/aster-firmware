"""Run the actual IMU bridge against fake hardware and bounded guest memory."""
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

class ImuBridge(unittest.TestCase):
    def test_coherence_bounds_errors_and_legacy(self):
        source = (ROOT / 'systems/zephyr/runtime/aster_native.c').read_text()
        bridge = source[source.index('#define IMU_CACHE_MS'):source.index('/* ============ camera')]
        harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <aster/aster.h>
typedef void *wasm_exec_env_t;
static union { uint64_t alignment; unsigned char bytes[256]; } memory;
static int64_t now;
static int features=1, fetches, hardware_error;
static int64_t k_uptime_get(void) { return now; }
static int aster_sysinfo_has_feature(int feature) { (void)feature; return features; }
static uint8_t *buf(wasm_exec_env_t env, uint32_t off, uint32_t size) {
    (void)env;return off && off <= 256 && size <= 256-off ? memory.bytes+off : 0;
}
static int aster_plat_imu_read(float *ax,float *ay,float *az,float *gx,float *gy,float *gz) {
    fetches++;if (hardware_error) return hardware_error;
    *ax=(float)fetches;*ay=2;*az=9.8f;*gx=.1f;*gy=.2f;*gz=.3f;return 0;
}
'''
        cases = r'''
int main(void) {
    aster_imu_sample_t *s=(void *)(memory.bytes+32);
    assert(n_imu_sample_read(0,0,sizeof(*s)) == ASTER_EINVAL);
    assert(n_imu_sample_read(0,250,sizeof(*s)) == ASTER_EINVAL);
    assert(n_imu_sample_read(0,32,sizeof(*s)-1) == ASTER_EINVAL);
    assert(fetches == 0);
    features=0;
    assert(n_imu_sample_read(0,32,sizeof(*s)) == ASTER_ENODEV);
    assert(fetches == 0);
    features=1;
    assert(n_imu_get_info(0,128,sizeof(aster_imu_info_t)) == 0);
    aster_imu_info_t *info=(void *)(memory.bytes+128);
    assert(info->version==1 && info->flags==3 && info->min_interval_ms==10);
    assert(n_imu_sample_read(0,32,sizeof(*s)) == 0 && fetches == 1);
    assert(s->sequence==1 && s->timestamp_ms==0 && s->accel[0]==1 && s->gyro[2]==.3f);
    now=9;
    assert(n_accel_read(0,192,12)==0 && *(float *)(memory.bytes+192)==1);
    assert(n_gyro_read(0,208,12)==0 && *(float *)(memory.bytes+208)==.1f);
    assert(fetches==1);
    now=10;
    assert(n_imu_sample_read(0,32,sizeof(*s))==0 && fetches==2);
    assert(s->sequence==2 && s->timestamp_ms==10 && s->accel[0]==2);
    aster_imu_sample_t saved=*s;
    hardware_error=-EIO;now=20;
    assert(n_imu_sample_read(0,32,sizeof(*s))==ASTER_EIO && fetches==3);
    assert(memcmp(s,&saved,sizeof(saved))==0); /* no partial/stale output */
    now=25;
    assert(n_imu_sample_read(0,32,sizeof(*s))==ASTER_EIO && fetches==3);
    hardware_error=-ENODEV;now=30;
    assert(n_imu_get_info(0,128,sizeof(*info))==ASTER_ENODEV);
    hardware_error=0;now=40;
    assert(n_imu_sample_read(0,32,sizeof(*s))==0 && s->sequence==3);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / 'imu')
            result = subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-x','c','-',
                '-I'+str(ROOT/'contract/include'),'-o',executable],
                input=harness+bridge+cases,text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([executable],check=True,timeout=5)
