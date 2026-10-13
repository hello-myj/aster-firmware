"""Actual range-based memory telemetry, including failed and oversized queries."""
import unittest
from test_waveshare import ROOT,run_c
class GuestMemoryStats(unittest.TestCase):
 def test_linear_bytes_and_failed_query_never_use_uninitialized_pool_data(self):
  s=(ROOT/'systems/zephyr/runtime/aster_runtime.c').read_text();fn=s[s.index('static void ctx_note_linear_memory('):s.index('static int call_fn(')]
  run_c(r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
typedef struct {void *module_inst;char *id;} aster_app_ctx_t;
static bool valid=true;static uint64_t begin,end=1048576;static uint32_t peak;static int calls;
static bool wasm_runtime_get_app_addr_range(void *inst,uint64_t offset,uint64_t *first,uint64_t *last){(void)inst;assert(offset==0);if(valid){*first=begin;*last=end;}return valid;}
static void aster_app_mgr_note_mem(const char *id,uint32_t n){assert(id);calls++;if(n>peak)peak=n;}
''' + fn + r'''
int main(void){aster_app_ctx_t c={.id="test.p2"};ctx_note_linear_memory(&c);assert(peak==1048576&&calls==1);
 end=2097152;ctx_note_linear_memory(&c);assert(peak==2097152&&calls==2);
 valid=false;ctx_note_linear_memory(&c);assert(calls==2);valid=true;begin=5;end=4;ctx_note_linear_memory(&c);assert(calls==2);
 begin=0;end=UINT64_C(4294967296);ctx_note_linear_memory(&c);assert(calls==2);return 0;}
''')
