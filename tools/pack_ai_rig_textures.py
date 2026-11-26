#!/usr/bin/env python3
"""Pack the five original-model texture atlases for the on-device mesh renderer."""
import argparse
from array import array
import json
from pathlib import Path
import struct
import sys

from PIL import Image


def pack(model: Path, output: Path):
    names = json.loads(model.read_text(encoding="utf-8-sig"))["FileReferences"]["Textures"]
    sizes = (2048, 896, 416, 512, 384)
    if len(names) != len(sizes):
        raise ValueError("expected five texture atlases")
    data = bytearray(struct.pack("<4sI", b"ATX1", len(names)))
    for name, size in zip(names, sizes):
        with Image.open(model.parent / name) as source:
            source.thumbnail((size, size), Image.Resampling.LANCZOS)
            image = source.convert("RGBA")
        width, height = image.size
        texture = image.tobytes()
        pixels = array("H")
        for i in range(0, len(texture), 4):
            red, green, blue = texture[i:i + 3]
            pixels.append((red >> 3) << 11 | (green >> 2) << 5 | blue >> 3)
        if sys.byteorder != "little":
            pixels.byteswap()
        coverage = bytearray((len(pixels) + 1) // 2)
        for i, a in enumerate(texture[3::4]):
            coverage[i // 2] |= ((a * 15 + 127) // 255) << (4 * (i % 2))
        data.extend(struct.pack("<II", width, height))
        data.extend(pixels.tobytes())
        data.extend(coverage)
        print(f"{name}: {width}x{height}")
    if len(data) > 8 * 1024 * 1024:
        raise ValueError("texture pack exceeds the P4 memory budget")
    output.write_bytes(data)
    print(f"{output.name}: {len(data)} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path, help="path to preview-original.model3.json")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    pack(args.model, args.output)
