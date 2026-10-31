#!/usr/bin/env python3
"""Compile bounded, interruptible WAMR AOT for ESP32-P4 (RV32 ILP32F)."""
from pathlib import Path
import argparse
import subprocess
import tempfile


def uleb(data, position):
    value = 0
    for shift in range(0, 35, 7):
        if position >= len(data):
            raise ValueError("truncated WASM integer")
        byte = data[position]
        position += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            if value > 0xffffffff:
                break
            return value, position
    raise ValueError("invalid WASM integer")


def encode(value):
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return out


def interruptible_wasm(data, max_pages=128):
    """WAMR 2.4.3 emits loop termination polls only for shared memories.

    Give the AOT compilation copy a bounded shared memory. No guest threads
    or imports are introduced, and the portable source file is unchanged.
    """
    if data[:8] != b"\0asm\x01\0\0\0" or not 1 <= max_pages <= 256:
        raise ValueError("expected WASM32 and a 1..256 page memory cap")
    out = bytearray(data[:8])
    position, found = 8, False
    while position < len(data):
        section = data[position]
        size, begin = uleb(data, position + 1)
        end = begin + size
        if end > len(data):
            raise ValueError("truncated WASM section")
        payload = data[begin:end]
        if section == 5:
            count, p = uleb(payload, 0)
            if count != 1 or found:
                raise ValueError("P4 AOT requires one defined memory")
            flags, p = uleb(payload, p)
            initial, p = uleb(payload, p)
            if flags not in (0, 1, 3):
                raise ValueError("unsupported WASM memory flags")
            maximum = max_pages
            if flags & 1:
                declared, p = uleb(payload, p)
                maximum = min(declared, max_pages)
            if initial > maximum or p != len(payload):
                raise ValueError("memory minimum exceeds the P4 cap")
            payload = bytes(encode(1) + encode(3) + encode(initial) + encode(maximum))
            found = True
        out.append(section)
        out += encode(len(payload)) + payload
        position = end
    if not found:
        raise ValueError("P4 AOT requires a defined WASM32 memory")
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wasm", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--wamrc", type=Path, default=Path(__file__).resolve().parents[2] / "aster-sdk/toolchain/wamrc")
    parser.add_argument("--max-pages", type=int, default=128)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="aster-p4-aot-") as directory:
        source = Path(directory) / "interruptible.wasm"
        source.write_bytes(interruptible_wasm(args.wasm.read_bytes(), args.max_pages))
        subprocess.run([str(args.wamrc), "--target=riscv32", "--target-abi=ilp32f", "--cpu=generic-rv32",
                        "--cpu-features=+m,+a,+f,+c", "--enable-multi-thread", "--bounds-checks=1",
                        "--stack-bounds-checks=1", "-o", str(args.output), str(source)], check=True)


if __name__ == "__main__":
    main()
