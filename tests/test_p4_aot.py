"""Validate bounded AOT preparation and rejection of noninterruptible profiles."""
import importlib.util
import sys
import unittest
from test_waveshare import ROOT, run_c

sys.path.insert(0, str(ROOT / "tools"))
from p4_aot import interruptible_wasm


class P4AOT(unittest.TestCase):
    def test_runtime_rejects_portable_code_before_starting_vm(self):
        source = (ROOT / "systems/espidf/components/aster_runtime/runtime.c").read_text()
        guard = source[source.index("#ifdef ASTER_REQUIRE_AOT"):
                       source.index("#ifdef ESP_PLATFORM\n    aster_idf_files_context")]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ASTER_REQUIRE_AOT 1
#define ASTER_EABI -13
static unsigned frees;
static void discard(void *p){assert(p);frees++;}
#define free discard
static int admit(uint8_t *bytes,uint32_t wasm_size){uint8_t *wasm_bytes=bytes;
''' + guard + r'''
 return 0;
}
int main(void){uint8_t aot[]={0,'a','o','t'},wasm[]={0,'a','s','m'};
 assert(admit(aot,4)==0&&frees==0);
 assert(admit(wasm,4)==-13&&frees==1);
 assert(admit(aot,3)==-13&&frees==2);return 0;}
''')

    def test_psram_code_requires_contiguous_instruction_mapping(self):
        source = (ROOT / "boards/osptek_p4/idf/port/wamr_memmap.c").read_text()
        mapping = source[source.index("void *os_mmap("):source.index("void *os_mremap(")]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define CONFIG_WAMR_ENABLE_AOT 1
#define MMAP_PROT_EXEC 4
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_OK 0
#define MMU_TARGET_PSRAM0 8
#define MMU_VADDR_INSTRUCTION 1
typedef uintptr_t esp_paddr_t;
typedef int mmu_target_t;
typedef int os_file_handle;
static void *allocated;static size_t allocated_size;static int fail,freed,calls;
static int esp_cache_get_alignment(int caps,size_t *a){assert(caps==3);*a=64;return fail==1?-1:0;}
static void *heap_caps_aligned_alloc(size_t a,size_t n,int caps){assert(caps==3);calls++;allocated_size=n;allocated=aligned_alloc(a,(n+a-1)&~(a-1));return allocated;}
static void heap_caps_free(void *p){freed++;free(p);}
static int esp_mmu_vaddr_to_paddr(void *p,esp_paddr_t *pa,mmu_target_t *t){*pa=(uintptr_t)p;*t=fail==2?0:8;if(fail==3&&p!=allocated)(*pa)--;return 0;}
static int esp_mmu_paddr_to_vaddr(esp_paddr_t p,mmu_target_t t,int kind,void **v){assert(t==8&&kind==1);*v=(void*)(p+(fail==4?16:0));return 0;}
''' + mapping + r'''
int main(void){
 void *p=os_mmap(NULL,129,4,0,0);assert(p&&allocated_size==192&&((uintptr_t)p%64)==0);
 for(unsigned i=0;i<192;i++)assert(((uint8_t*)p)[i]==0);heap_caps_free(p);
 for(fail=1;fail<=4;fail++){int before=freed;assert(!os_mmap(NULL,129,4,0,0));assert(freed==before+(fail!=1));}
 fail=0;int before=calls;assert(!os_mmap(NULL,SIZE_MAX,4,0,0));assert(calls==before);
 assert(!os_mmap(NULL,0,4,0,0));
 return 0;
}
''')

    def test_defined_memory_is_bounded_shared_without_changing_source(self):
        original = b"\0asm\x01\0\0\0\x05\x03\x01\x00\x01"
        expected = b"\0asm\x01\0\0\0\x05\x05\x01\x03\x01\x80\x01"
        self.assertEqual(interruptible_wasm(original), expected)
        self.assertEqual(original[-3:], b"\x01\x00\x01")
        self.assertEqual(interruptible_wasm(expected), expected)

    def test_sdk_profile_matches_firmware_tool(self):
        helper = ROOT.parent / "aster-sdk/tools/aot_memory.py"
        if not helper.is_file():
            self.skipTest("sibling SDK is optional")
        spec = importlib.util.spec_from_file_location("sdk_aot_memory", helper)
        sdk = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(sdk)
        source = b"\0asm\x01\0\0\0\x05\x04\x01\x01\x01\x40"
        for cap in (1, 16, 64, 128, 256):
            self.assertEqual(interruptible_wasm(source, cap), sdk.interruptible_wasm(source, cap))

    def test_invalid_or_unbounded_memory_is_refused(self):
        for data in (b"bad", b"\0asm\x01\0\0\0", b"\0asm\x01\0\0\0\x05\xff",
                     b"\0asm\x01\0\0\0\x05\x03\x01\x00\x7f"):
            with self.assertRaises(ValueError):
                interruptible_wasm(data, 16)

    def test_loader_profile_rejects_truncation_bad_flags_and_excessive_memory(self):
        path = (ROOT / "systems/espidf/components/aster_runtime/aot_profile.c").as_posix()
        run_c('#include "' + path + '"\n' + r'''
#include <assert.h>
static void put(uint8_t *p,uint32_t v) {for(int i=0;i<4;i++)p[i]=v>>(i*8);}
int main(void) {
 uint8_t bytes[96]={0};memcpy(bytes,"\0aot",4);put(bytes+4,6);
 put(bytes+8,0);put(bytes+12,48);put(bytes+16+16,4);
 put(bytes+64,1);put(bytes+68,24);put(bytes+72,0);put(bytes+76,1);
 put(bytes+80,3);put(bytes+84,65536);put(bytes+88,4);put(bytes+92,128);
 assert(aster_idf_aot_interruptible(bytes,sizeof(bytes)));
 for(size_t i=0;i<sizeof(bytes);i++)assert(!aster_idf_aot_interruptible(bytes,i));
 put(bytes+80,0);assert(!aster_idf_aot_interruptible(bytes,sizeof(bytes)));put(bytes+80,3);
 put(bytes+32,0);assert(!aster_idf_aot_interruptible(bytes,sizeof(bytes)));put(bytes+32,4);
 put(bytes+92,257);assert(!aster_idf_aot_interruptible(bytes,sizeof(bytes)));put(bytes+92,2);
 assert(!aster_idf_aot_interruptible(bytes,sizeof(bytes)));put(bytes+92,128);
 put(bytes+68,0xffffffffu);assert(!aster_idf_aot_interruptible(bytes,sizeof(bytes)));
 return 0;
}
''')
