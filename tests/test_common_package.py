"""Compile the portable package decoder without either OS's include paths."""
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CommonPackage(unittest.TestCase):
    def test_device_policy_and_allocation_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            for target in ("xtensa", "riscv"):
                manifest = json.dumps({"product": "aster", "target_id": target,
                    "id": "test.shared", "name": "Shared", "version": "1.0.0",
                    "abi": {"min": "4.3"}, "requires": ["display"],
                    "orientation": "landscape"}).encode()
                wasm = (64 + len(manifest) + 3) & ~3
                image = bytearray(wasm + 8)
                image[:4] = b"ASTP"
                struct.pack_into("<HH", image, 4, 1, 64)
                struct.pack_into("<4I", image, 12, 64, len(manifest), wasm, 8)
                image[64:64+len(manifest)] = manifest
                image[wasm:] = b"\0asm\1\0\0\0"
                (base / (target + ".ast")).write_bytes(image)
            source = base / "test.c"
            source.write_text(r'''
#include "aster_pkg_format.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int live,oom;static bool supported=true;
static void *allocate(size_t n){if(oom)return NULL;void *p=malloc(n);if(p)live++;return p;}
static void release(void *p){if(p)live--;free(p);}
static bool feature(const char *name){assert(!strcmp(name,"display"));return supported;}
int main(int argc,char **argv){
 assert(argc==3);
 const char *targets[]={"xtensa","riscv"};
 for(int i=0;i<2;i++){
  FILE *f=fopen(argv[i+1],"rb");assert(f);uint8_t image[2048];size_t n=fread(image,1,sizeof(image),f);fclose(f);
  aster_pkg_policy_t p={.target_id=targets[i],.abi_version=0x040300,.alloc=allocate,.release=release,.has_feature=feature};
  aster_manifest_t m;const uint8_t *wasm=NULL;size_t length=0;
  assert(!aster_pkg_decode(&p,image,n,&m,&wasm,&length,NULL,NULL,NULL));
  assert(!strcmp(m.id,"test.shared") && m.orientation==ASTER_ORIENT_LANDSCAPE);
  assert(length==8 && !memcmp(wasm,"\0asm",4) && live==0);
  p.target_id=targets[1-i];assert(aster_pkg_decode(&p,image,n,&m,0,0,0,0,0)==ASTER_EABI);assert(!live);
  p.target_id=targets[i];p.abi_version=0x040200;assert(aster_pkg_decode(&p,image,n,&m,0,0,0,0,0)==ASTER_EABI);
  p.abi_version=0x040300;supported=false;assert(aster_pkg_decode(&p,image,n,&m,0,0,0,0,0)==ASTER_ENOTSUP);assert(!live);
  supported=true;oom=1;assert(aster_pkg_decode(&p,image,n,&m,0,0,0,0,0)==ASTER_ENOMEM);assert(!live);oom=0;
  assert(aster_pkg_decode(&p,image,63,&m,0,0,0,0,0)==ASTER_ECORRUPT);
  assert(aster_pkg_decode(NULL,image,n,&m,0,0,0,0,0)==ASTER_EINVAL);
 }
 assert(!aster_pkg_in_bounds(UINT32_MAX,32,4096,64));
 assert(!aster_pkg_in_bounds(64,SIZE_MAX,4096,64));
 assert(aster_pkg_in_bounds(64,32,96,64));
 return 0;
}
''', encoding="utf-8")
            binary = base / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-g",
                "-I"+str(ROOT/"common/include"), "-I"+str(ROOT/"common/package"),
                "-I"+str(ROOT/"contract/include"), str(source),
                str(ROOT/"common/package/aster_pkg_decode.c"), "-o", str(binary)],
                check=True, capture_output=True, text=True)
            subprocess.run([str(binary), str(base/"xtensa.ast"), str(base/"riscv.ast")],
                           check=True, capture_output=True, text=True, timeout=10)

    def test_common_has_no_backend_dependencies(self):
        for file in (ROOT/"common").rglob("*"):
            if file.suffix not in (".c", ".h"):
                continue
            source = file.read_text()
            for forbidden in ("<zephyr/", '"zephyr/', "<freertos/", "<esp_", "systems/", "CONFIG_"):
                self.assertNotIn(forbidden, source, str(file))
