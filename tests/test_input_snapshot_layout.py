"""The native polling import writes bounded ABI 4.0 12-byte touch records."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = re.search(r'^(?:static )?int32_t ' + re.escape(name) + r'\(', source, re.M).start()
    brace = source.index('{', start)
    depth = 1
    i = brace + 1
    while depth:
        depth += (source[i] == '{') - (source[i] == '}')
        i += 1
    return source[start:i] + '\n'


class InputSnapshotLayout(unittest.TestCase):
    def test_native_poll_is_12_bytes_bounded_and_compacts_valid_points(self):
        source = (ROOT / "systems/zephyr/runtime/aster_native.c").read_text()
        code = r'''
#include <aster/error.h>
#include <aster/input.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define ASTER_TOUCH_MAX 2
#define ASTER_INPUT_TERM 0xffffffffu
typedef struct Env { uint8_t memory[96]; } *wasm_exec_env_t;
typedef struct { int unused; } aster_app_ctx_t;
static aster_app_ctx_t app;
static unsigned advertised=2, state_calls, buf_calls;
static int map_reject_x;
static uint32_t live[8];
static aster_app_ctx_t *ctx_of(wasm_exec_env_t env) { return env ? &app : NULL; }
static uint8_t *buf(wasm_exec_env_t env,uint32_t off,size_t len) {
    buf_calls++;
    if(!env || off>sizeof(env->memory) || len>sizeof(env->memory)-off) return NULL;
    return env->memory+off;
}
static uint32_t aster_plat_input_touch_max_points(void) { return advertised; }
static void aster_plat_input_state(uint32_t *out,uint32_t max) {
    state_calls++;
    for(uint32_t i=0;i<max*4;i++)out[i]=live[i];
}
static bool aster_plat_display_app_touch(uint32_t *x,uint32_t *y) {
    (void)y;return (int)*x!=map_reject_x;
}
static aster_touch_point_t point_at(wasm_exec_env_t env,uint32_t off) {
    aster_touch_point_t p;memcpy(&p,env->memory+off,sizeof(p));return p;
}
''' + function(source,'n_input_touch') + r'''
int main(void) {
    struct Env env; memset(&env,0xa5,sizeof(env));
    aster_touch_point_t p;
    unsigned old_buf=buf_calls,old_state=state_calls;
    assert(n_input_touch(&env,90,0)==0 && buf_calls==old_buf && state_calls==old_state);

    live[0]=11;live[1]=22;live[2]=1;live[3]=0;live[4]=ASTER_INPUT_TERM;
    map_reject_x=-1;
    assert(n_input_touch(&env,1,1)==1);
    p=point_at(&env,1);assert(p.x==11&&p.y==22&&p.action==1);
    assert(env.memory[13]==0xa5); /* exact 12-byte write at an unaligned offset */

    live[0]=11;live[1]=22;live[2]=1;live[3]=0;
    live[4]=33;live[5]=44;live[6]=1;live[7]=1;
    memset(env.memory,0xa5,sizeof(env.memory));
    assert(n_input_touch(&env,17,2)==2);
    p=point_at(&env,17);assert(p.x==11&&p.y==22&&p.action==1);
    p=point_at(&env,29);assert(p.x==33&&p.y==44&&p.action==1);
    assert(env.memory[41]==0xa5); /* no 16-byte slot write or extra terminator */

    /* With max=1, discard an invalid first panel point and keep the second. */
    live[0]=99;live[1]=1;live[2]=1;live[3]=0;
    live[4]=77;live[5]=2;live[6]=1;live[7]=1;
    map_reject_x=99;memset(env.memory,0xa5,sizeof(env.memory));
    assert(n_input_touch(&env,3,1)==1);
    p=point_at(&env,3);assert(p.x==77&&p.y==2&&p.action==1);
    assert(env.memory[15]==0xa5);

    memset(env.memory,0xa5,sizeof(env.memory));
    map_reject_x=-1;
    assert(n_input_touch(&env,5,2)==2);
    assert(env.memory[29]==0xa5);
    old_state=state_calls;
    assert(n_input_touch(&env,95,1)==ASTER_EINVAL && state_calls==old_state);
    advertised=0;old_buf=buf_calls;old_state=state_calls;
    assert(n_input_touch(&env,95,1)==0 && buf_calls==old_buf && state_calls==old_state);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "input-snapshot"
            result = subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "contract/include"), "-x", "c", "-", "-o", str(exe)],
                input=code, text=True, capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run([str(exe)], text=True, capture_output=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
