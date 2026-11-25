"""Check the native avatar pack against the preview's RGB565A8 planes."""

import contextlib
import gzip
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from pack_ai_avatar import CLIP, HEADER, PIXELS, SIZE, pack


class AvatarPack(unittest.TestCase):
    def test_trimmed_planes_keep_source_coordinates_and_clip_index(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp)
            first = bytearray(PIXELS * 3)
            second = bytearray(PIXELS * 3)
            for frame, x, y, color in ((first, 8, 11, 0xF800), (second, 9, 12, 0x001F)):
                pixel = y * SIZE + x
                struct.pack_into("<H", frame, pixel * 2, color)
                frame[PIXELS * 2 + pixel] = 255
            (source / "idle.gz").write_bytes(gzip.compress(first + second))
            (source / "still.gz").write_bytes(gzip.compress(second))
            (source / "media.json").write_text(json.dumps({
                "width": SIZE, "height": SIZE, "fps": 15,
                "motions": [{"path": "idle.gz", "name": "待机", "frames": 2, "loop": True}],
                "expressions": [{"path": "still.gz", "name": "前倾", "frames": 1}],
            }), encoding="utf-8")
            output = source / "ai-avatar.bin"
            with contextlib.redirect_stdout(io.StringIO()):
                pack(source, output)
            raw = output.read_bytes()
            self.assertEqual(HEADER.unpack_from(raw)[:10], (b"AWA1", 460, 460, 8, 11, 2, 2, 15, 2, 3))
            self.assertEqual(CLIP.unpack_from(raw, HEADER.size)[:3], (0, 2, 1))
            self.assertEqual(CLIP.unpack_from(raw, HEADER.size + CLIP.size)[:3], (2, 1, 0))
            offsets = struct.unpack_from("<4I", raw, HEADER.size + 2 * CLIP.size)
            self.assertEqual(offsets[-1], len(raw))
            decoded = [zlib.decompress(raw[a:b]) for a, b in zip(offsets, offsets[1:])]
            self.assertEqual(decoded, [
                b"\x00\xf8\0\0\0\0\0\0\xff\0\0\0",
                b"\0\0\0\0\0\0\x1f\x00\0\0\0\xff",
                b"\0\0\0\0\0\0\x1f\x00\0\0\0\xff",
            ])
