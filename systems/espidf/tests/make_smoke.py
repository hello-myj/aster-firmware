#!/usr/bin/env python3
"""Generate a core lifecycle smoke WASM without requiring the SDK/toolchain."""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]


def leb(value):
    result = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        done = (value == 0 and byte < 64) or (value == -1 and byte >= 64)
        result.append(byte if done else byte | 128)
        if done:
            return bytes(result)


def name(value):
    data = value.encode()
    return bytes([len(data)]) + data


def section(kind, data):
    # Section sizes use unsigned LEB128.
    size = len(data)
    encoded = bytearray()
    while size >= 128:
        encoded.append((size & 127) | 128)
        size >>= 7
    encoded.append(size)
    return bytes([kind]) + encoded + data


def module():
    header = (ROOT / "contract/include/aster/system.h").read_text()
    v = [int(re.search(r"#define\s+ASTER_ABI_" + part + r"\s+(\d+)", header)[1])
         for part in ("MAJOR", "MINOR", "PATCH")]
    abi = (v[0] << 16) | (v[1] << 8) | v[2]
    data = b"\0asm\x01\0\0\0"
    data += section(1, bytes.fromhex("03 60 00 01 7f 60 00 00 60 01 7f 00"))
    data += section(2, b"\x02" + name("env") + name("aster_system_get_abi_version") + b"\0\0"
                    + name("env") + name("aster_app_exit") + b"\0\x02")
    data += section(3, bytes([5, 0, 1, 2, 1, 1]))
    data += section(5, bytes([1, 0, 1]))
    exports = [name("memory") + bytes([2, 0])]
    for index, suffix in enumerate(("init", "start", "event", "stop", "destroy"), 2):
        exports.append(name("aster_app_" + suffix) + bytes([0, index]))
    data += section(7, bytes([len(exports)]) + b"".join(exports))
    # init checks ABI; event exits immediately; all other lifecycle exports are no-ops.
    bodies = [b"\0\x10\0\x41" + leb(abi) + bytes.fromhex("46 04 7f 41 00 05 41 7f 0b 0b"),
              b"\0\x0b", bytes.fromhex("00 41 00 10 01 0b"), b"\0\x0b", b"\0\x0b"]
    data += section(10, bytes([len(bodies)]) + b"".join(bytes([len(b)]) + b for b in bodies))
    return data


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/osptek-p4/sdcard/app.wasm")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(module())
    print(args.output)
