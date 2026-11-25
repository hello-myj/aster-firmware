#!/usr/bin/env python3
"""Pack the 460px LVGL preview's RGB565A8 frames for the FAKE POD SD card."""

import argparse
import gzip
import json
from pathlib import Path
import struct
import zlib


SIZE = 460
PIXELS = SIZE * SIZE
HEADER = struct.Struct("<4s6H4I")
CLIP = struct.Struct("<IHH48s")
MAX_PACK_BYTES = 32 * 1024 * 1024


def frames(source, clip):
    path = (source / clip["path"]).resolve()
    if not path.is_relative_to(source):
        raise ValueError("Avatar media path leaves the preview directory")
    with gzip.open(path, "rb") as media:
        for _ in range(clip["frames"]):
            frame = media.read(PIXELS * 3)
            if len(frame) != PIXELS * 3:
                raise ValueError(f"Truncated avatar frame in {path.name}")
            yield frame
        if media.read(1):
            raise ValueError(f"Unexpected extra frames in {path.name}")


def bounds(source, clips):
    left, top, right, bottom = SIZE, SIZE, 0, 0
    for clip in clips:
        for frame in frames(source, clip):
            for y in range(SIZE):
                start = PIXELS * 2 + y * SIZE
                row = frame[start : start + SIZE].rstrip(b"\0")
                if row:
                    left = min(left, len(row) - len(row.lstrip(b"\0")))
                    right = max(right, len(row))
                    top = min(top, y)
                    bottom = max(bottom, y + 1)
    if right <= left or bottom <= top:
        raise ValueError("Avatar contains no visible pixels")
    return left, top, right - left, bottom - top


def crop(frame, x, y, width, height):
    view = memoryview(frame)
    color = [view[(row * SIZE + x) * 2 : (row * SIZE + x + width) * 2]
             for row in range(y, y + height)]
    alpha = [view[PIXELS * 2 + row * SIZE + x : PIXELS * 2 + row * SIZE + x + width]
             for row in range(y, y + height)]
    return b"".join(color + alpha)


def pack(source, output):
    source = source.resolve(strict=True)
    data = json.loads((source / "media.json").read_text(encoding="utf-8-sig"))
    if (data["width"], data["height"], data["fps"]) != (SIZE, SIZE, 15):
        raise ValueError("Expected the square 460px, 15 FPS preview")
    clips = data["motions"] + data["expressions"]
    if not 1 <= len(clips) <= 64 or any(not 1 <= c["frames"] <= 2047 for c in clips):
        raise ValueError("Invalid number of avatar clips or frames")
    frame_count = sum(c["frames"] for c in clips)
    if frame_count > 2048:
        raise ValueError("Too many avatar frames")
    x, y, width, height = bounds(source, clips)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".part")
    offsets, largest = [], 0
    try:
        with temporary.open("wb") as destination:
            destination.write(b"\0" * HEADER.size)
            first = 0
            for clip in clips:
                label = clip["name"].encode("utf-8")
                if not label or len(label) >= 48 or b"\0" in label:
                    raise ValueError(f"Invalid avatar clip label: {clip['name']}")
                destination.write(CLIP.pack(first, clip["frames"], bool(clip.get("loop")), label))
                first += clip["frames"]
            index = destination.tell()
            destination.write(b"\0" * (4 * (frame_count + 1)))
            for clip in clips:
                for frame in frames(source, clip):
                    compressed = zlib.compress(crop(frame, x, y, width, height), level=6)
                    largest = max(largest, len(compressed))
                    offsets.append(destination.tell())
                    destination.write(compressed)
            offsets.append(destination.tell())
            if offsets[-1] > MAX_PACK_BYTES:
                raise ValueError("Avatar exceeds the device's 32 MiB resource limit")
            destination.seek(index)
            destination.write(struct.pack(f"<{len(offsets)}I", *offsets))
            destination.seek(0)
            destination.write(HEADER.pack(b"AWA1", SIZE, SIZE, x, y, width, height,
                                          data["fps"], len(clips), frame_count, largest))
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"{len(clips)} clips, {frame_count} frames, {width}x{height} crop, {offsets[-1]:,} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="460px LVGL preview directory")
    parser.add_argument("output", type=Path, help="SD card resource file")
    args = parser.parse_args()
    pack(args.source, args.output)
