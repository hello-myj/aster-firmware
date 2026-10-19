"""Frozen old SDK packages through the real firmware C parser on the host."""
import json, os, subprocess, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class OldPackages(unittest.TestCase):
 def test_real_parser_accepts_frozen_ast_and_rejects_wrong_version(self):
  pkg=(ROOT/'systems/zephyr/services/aster_pkg.c').read_text()
  impl=pkg[pkg.index('static bool in_bounds('):pkg.index('/* Load a whole file into memory */')]
  source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <aster/aster.h>
#include "aster_pkg.h"
#include "aster_bytes.h"
#define k_malloc malloc
#define k_free free
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define CONFIG_ASTER_TARGET_ID "xtensa"
static bool aster_sysinfo_feature_by_name(const char *name) {(void)name;return true;}
''' + impl + r'''
int main(int argc,char **argv) {
 for(int i=1;i<argc;i++) {
  FILE *f=fopen(argv[i],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);rewind(f);
  unsigned char *p=malloc(n);assert(p);assert(fread(p,1,n,f)==(size_t)n);assert(!fclose(f));
  aster_manifest_t m;const uint8_t *wasm,*aot;size_t wl,al;uint32_t at;
  assert(aster_pkg_parse(p,n,&m,&wasm,&wl,&aot,&al,&at)==ASTER_EABI);
  p[4]=99;assert(aster_pkg_parse(p,n,&m,0,0,0,0,0)==ASTER_ECORRUPT);free(p);
 }
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   c=Path(d)/'parse.c';exe=Path(d)/'parse';c.write_text(source)
   result=subprocess.run(['cc','-std=c11','-Werror','-Wall','-Wextra','-I'+str(ROOT/'contract/include'),'-I'+str(ROOT/'systems/zephyr/services'),'-I'+str(ROOT/'systems/zephyr/core'),str(c),'-o',str(exe)],text=True,capture_output=True)
   self.assertEqual(result.returncode,0,result.stderr)
   base=ROOT/'tests/fixtures/compatibility';items=json.loads((base/'manifest.json').read_text())['packages']
   paths=[str(base/i['file']) for i in items]
   paths.extend(json.loads(os.environ.get('ASTER_COMPAT_PACKAGES','[]')))
   subprocess.run([str(exe),*paths],check=True,timeout=10)
