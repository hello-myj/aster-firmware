"""Convert this headless preview's P6 frames to PNG using only stdlib."""
from pathlib import Path
import struct
import sys
import zlib

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

for source in Path(sys.argv[1]).glob('*.ppm'):
    magic, size, maximum, rgb = source.read_bytes().split(b'\n', 3)
    width, height = map(int, size.split())
    assert magic == b'P6' and maximum == b'255' and len(rgb) == width * height * 3
    rows = b''.join(b'\0' + rgb[y*width*3:(y+1)*width*3] for y in range(height))
    source.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
