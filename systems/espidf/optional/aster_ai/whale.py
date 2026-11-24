"""Rasterize Little Whale as LVGL RGB565A8 (little-endian color plane, then alpha)."""
import math
import struct
import sys
from pathlib import Path

WIDTH, HEIGHT = 144, 96
pixels = [bytearray(WIDTH) for _ in range(HEIGHT)]


def shape(color, start, *curves):
    """Fill a closed cubic path at pixel centers, using even/odd scanlines."""
    points = [start]
    for x1, y1, x2, y2, x3, y3 in curves:
        x0, y0 = points[-1]
        for step in range(1, 17):
            t = step / 16
            u = 1 - t
            points.append((u**3*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t**3*x3,
                           u**3*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t**3*y3))
    edges = list(zip(points, points[1:] + points[:1]))
    for y, row in enumerate(pixels):
        height = y + 0.5
        crossings = sorted(x1 + (height-y1)*(x2-x1)/(y2-y1)
                           for (x1, y1), (x2, y2) in edges
                           if min(y1, y2) <= height < max(y1, y2))
        for left, right in zip(crossings[::2], crossings[1::2]):
            a, b = max(0, math.ceil(left-0.5)), min(WIDTH, math.ceil(right-0.5))
            row[a:b] = bytes([color]) * (b-a)


def dot(color, cx, cy, rx, ry):
    for y, row in enumerate(pixels):
        for x in range(WIDTH):
            if ((x+0.5-cx)/rx)**2 + ((y+0.5-cy)/ry)**2 <= 1:
                row[x] = color


# Tail, body, cream underside and the foreground pectoral fin.
shape(2, (37, 58), (21, 58, 8, 49, 5, 34), (14, 33, 22, 36, 28, 44),
      (28, 32, 35, 27, 42, 26), (46, 39, 40, 49, 37, 58))
shape(1, (28, 53), (43, 60, 39, 30, 72, 23), (98, 16, 125, 26, 133, 46),
      (146, 76, 113, 85, 88, 84), (57, 85, 36, 73, 28, 53))
shape(3, (35, 60), (57, 73, 77, 75, 99, 66), (111, 62, 125, 59, 135, 58),
      (134, 78, 112, 85, 88, 84), (63, 84, 44, 76, 35, 60))
shape(2, (71, 62), (79, 67, 85, 73, 85, 80), (75, 80, 64, 73, 62, 66),
      (65, 66, 68, 64, 71, 62))
shape(4, (54, 34), (64, 25, 83, 22, 94, 24), (84, 26, 72, 26, 60, 34),
      (57, 36, 54, 36, 54, 34))
dot(5, 114, 46, 4, 5)
dot(6, 115, 44, 1.25, 1.25)
dot(7, 113, 55, 5, 2)
shape(5, (123, 55), (126, 58, 129, 57, 131, 54), (130, 60, 125, 61, 123, 55))
# A tiny fountain gives the silhouette its own character even at thumbnail size.
shape(8, (84, 19), (83, 13, 77, 9, 75, 8), (78, 4, 85, 9, 85, 15),
      (86, 8, 91, 4, 95, 6), (96, 10, 88, 11, 86, 19))
dot(8, 70, 5, 2, 2)
dot(4, 99, 3, 2, 2)

palette = [0, 0xff568bdf, 0xff3864ad, 0xffe4eeda, 0xff90c4fc, 0xff182d48,
           0xfffff5df, 0xfff3aa87, 0xff86dbc9]


def rgb565(argb):
    red = (argb >> 19) & 0x1f
    green = (argb >> 10) & 0x3f
    blue = (argb >> 3) & 0x1f
    return (red << 11) | (green << 5) | blue


colors = [struct.pack("<H", rgb565(argb)) for argb in palette]
indices = b"".join(pixels)
color_plane = b"".join(colors[i] for i in indices)
alpha_plane = bytes(palette[i] >> 24 for i in indices)
Path(sys.argv[1]).write_bytes(color_plane + alpha_plane)
