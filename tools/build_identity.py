#!/usr/bin/env python3
"""Identify a full board build; this never implies that it was flashed/tested."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from check_target import check
from release_check import ROOT, identity
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--board',required=True);a=p.parse_args()
check(a.board,a.build_dir)
files=['zephyr/zephyr.bin','zephyr/.config','CMakeCache.txt']
result={'schema':1,'board':a.board,'source':identity(ROOT),'hardware_test':'not_run','sha256':{f:hashlib.sha256((a.build_dir/f).read_bytes()).hexdigest() for f in files}}
# Keep compiler, SDK and source revision references from the generated cache.
cache=(a.build_dir/'CMakeCache.txt').read_text();result['toolchain_cache']=[line for line in cache.splitlines() if any(key in line for key in ['ZEPHYR_SDK','CMAKE_C_COMPILER:','ZEPHYR_TOOLCHAIN_VARIANT:'])]
(a.build_dir/'identity.json').write_text(json.dumps(result,indent=2)+'\n')
