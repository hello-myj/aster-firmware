"""Validate bounded AOT preparation and rejection of noninterruptible profiles."""
import importlib.util
import sys
import unittest
from test_waveshare import ROOT, run_c

sys.path.insert(0, str(ROOT / "tools"))
from p4_aot import interruptible_wasm


class P4AOT(unittest.TestCase):
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
