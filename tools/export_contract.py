#!/usr/bin/env python3
"""Publish an immutable ABI/target bundle. Never writes into an SDK tree."""
import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def abi_version():
    """The ABI triple, read from the header that defines it. Derived rather
    than restated: the bundle's api.json, lock.json and the default output
    filename all have to agree with system.h, and a second spelling of the
    number is a second thing to forget."""
    version = (ROOT / "contract/include/aster/system.h").read_text()
    return ".".join(re.search(r'#define\s+ASTER_ABI_' + key + r'\s+(\d+)', version)[1]
                    for key in ("MAJOR", "MINOR", "PATCH"))


def bundle_files():
    files = {str(p.relative_to(ROOT / "contract")): p.read_bytes()
             for p in (ROOT / "contract/include").rglob("*.h")}
    native = (ROOT / "systems/zephyr/runtime/aster_native.c").read_text()
    signatures = dict(re.findall(r'\{\s*"(aster_[a-z0-9_]+)"\s*,[^,]+,\s*"([^"]+)"', native))
    libc = (ROOT / "modules/wamr/core/iwasm/libraries/libc-builtin/libc_builtin_wrapper.c").read_text()
    table = libc.split("native_symbols_libc_builtin[] = {", 1)[1].split("};", 1)[0]
    names = set(re.findall(r'REG_NATIVE_FUNC\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,', table))
    names.update(re.findall(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,', table))
    version = (ROOT / "contract/include/aster/system.h").read_text()
    abi = abi_version()
    api = {"schema": 1, "product": "aster", "abi": abi,
           "imports": {"env": sorted(names | signatures.keys())},
           "signatures": signatures, "package_format": "ASTP",
           "runtime": {"name": "wamr", "version": "2.4.3"},
           "aot_backends": {"xtensa": "xtensa-esp32s3", "riscv32": "riscv",
                            "thumbv7": "thumb-v7", "thumbv8": "cortex-m", "x86_64": "x86-64"}}
    feature_names = {"AUDIO_INPUT": "audio-in", "AUDIO_OUTPUT": "audio-out", "AUDIO_STREAM": "audio-stream"}
    api["feature_bits"] = {feature_names.get(name, name.lower()): int(bit)
                           for name, bit in re.findall(r"#define\s+ASTER_FEATURE_(\w+)\s+(\d+)u", version)}
    api["feature_bits"]["storage.app"] = api["feature_bits"]["filesystem"]
    files["contract/api.json"] = (json.dumps(api, indent=2) + "\n").encode()
    for path in (ROOT / "hardware").glob("*.json"):
        public = json.loads(path.read_text())["public"]
        files[f"targets/{path.name}"] = (json.dumps(public, indent=2) + "\n").encode()
    lock = {"schema": 1, "product": "aster", "abi": abi,
            "sha256": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(files.items())}}
    files["contract/lock.json"] = (json.dumps(lock, indent=2) + "\n").encode()
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=ROOT / f"dist/aster-contract-{abi_version()}.zip")
    args = parser.parse_args()
    from check_docs import check
    check(ROOT)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(bundle_files().items()):
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            archive.writestr(info, data)
    print(args.output)


if __name__ == "__main__":
    main()
