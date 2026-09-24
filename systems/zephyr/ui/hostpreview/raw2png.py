#!/usr/bin/env python3
"""RGB565LE raw -> PNG (upscaled) for the host-side gfx shape preview."""
import struct
import sys
import zlib
from pathlib import Path


def png(w, h, rgb_rows, path):
    raw = b"".join(b"\x00" + r for r in rgb_rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    hdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) +
                           chunk(b"IEND", b""))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    w, h = int(sys.argv[3]), int(sys.argv[4])
    scale = int(sys.argv[5]) if len(sys.argv) > 5 else 1
    buf = Path(src).read_bytes()
    assert len(buf) == w * h * 2, (len(buf), w * h * 2)

    base = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            v = buf[2 * (y * w + x)] | (buf[2 * (y * w + x) + 1] << 8)
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            row += bytes(((r * 255) // 31, (g * 255) // 63, (b * 255) // 31))
        base.append(bytes(row))

    if scale > 1:
        rows = []
        for r in base:
            big = b"".join(r[i:i + 3] * scale for i in range(0, len(r), 3))
            for _ in range(scale):
                rows.append(big)
        png(w * scale, h * scale, rows, dst)
    else:
        png(w, h, base, dst)
    print(f"{dst}: {w*scale}x{h*scale}")


if __name__ == "__main__":
    main()
