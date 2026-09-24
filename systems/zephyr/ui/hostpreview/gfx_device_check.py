#!/usr/bin/env python3
"""Geometric checks on a device gfx-frame capture of gfxshow.

gfxshow's scene is fixed geometry (see sdk/apps/gfxshow/src/main.c), so the
stroke rings and the rounded panel can be probed analytically:
  * circle ring at (182, 256) r=30  -> reddish on the whole circumference
  * its interior                    -> no reddish pixel (the hole is punched)
  * rounded panel outline           -> light grey ring, corners cut
  * scene is neither blank nor flat
A "ring missing" bug shows up as panel/background colour all along the ring
radius; a "gap" bug leaves an angular run with no ring pixel.

Note the ring is 1 px wide and antialiased, so a pixel on the analytic radius
is often a partial blend: match by hue dominance (r > g, b > g), not by
distance to the exact stroke colour, and allow +-1.5 px radially.

usage: gfx_device_check.py shot.png
"""
import math
import sys

from PIL import Image

STROKE = (244, 63, 94)
PANEL = (28, 34, 52)
GREY = (120, 130, 160)

CX, CY, R = 182, 256, 30
N = 72


def reddish(c):
    """Stroke hue dominance. A 1 px AA ring lands on 25%-covered pixels in
    places (r=82 vs the full 244), so this is deliberately loose; it still
    rejects the panel fill (28,34,52) and the gradient background (16,28,66)."""
    r, g, b = c

    return r > 60 and r > g + 30 and b > g


def greyish(c, tol=90):
    return sum(abs(a - b) for a, b in zip(c, GREY)) <= tol


def near(c, ref, tol=60):
    return sum(abs(a - b) for a, b in zip(c, ref)) <= tol


def main():
    im = Image.open(sys.argv[1]).convert("RGB")
    W, H = im.size
    px = im.load()
    print(f"image {W}x{H}")
    fail = 0

    def report(ok, label, extra=""):
        nonlocal fail

        if not ok:
            fail += 1
        print(f"  {'OK  ' if ok else 'FAIL'} {label} {extra}")

    # ---- circle stroke: red somewhere within +-1.5 px of the radius ----
    miss = []
    for i in range(N):
        a = 2 * math.pi * i / N
        ca, sa = math.cos(a), math.sin(a)
        for dr in (-1.5, -1.0, -0.5, 0.0, 0.5, 1.0, 1.5):
            x = int(round(CX + (R + dr) * ca))
            y = int(round(CY + (R + dr) * sa))
            if reddish(px[x, y]):
                break
        else:
            miss.append(i)
    report(not miss, f"circle ring present on {N} angles",
           f"({N - len(miss)}/{N} hit, gaps at {miss[:6]})")

    # ---- the ring must be a closed 8-connected loop ----
    pts = set()
    for y in range(CY - R - 2, CY + R + 3):
        for x in range(CX - R - 2, CX + R + 3):
            if reddish(px[x, y]):
                pts.add((x, y))
    deg = {p: sum(1 for dx in (-1, 0, 1) for dy in (-1, 0, 1)
                  if (dx or dy) and (p[0] + dx, p[1] + dy) in pts) for p in pts}
    ends = [p for p, d in deg.items() if d < 2]
    report(not ends, "ring is a closed loop",
           f"({len(pts)} px, {len(ends)} loose ends)")

    # ---- interior must be empty of the stroke colour: an inscribed square
    # (half-extent 15 < r/sqrt(2) = 21.2) so its corners cannot poke through
    # the ring and be mistaken for a stroked interior ----
    inner = sum(1 for y in range(CY - 15, CY + 16)
                for x in range(CX - 15, CX + 16) if reddish(px[x, y]))
    report(inner == 0, "ring interior is not stroked", f"({inner} px)")

    # ---- rounded panel outline: top edge + corners cut ----
    x0, y0, x1, y1 = 8, H - 118, W - 8, H - 8
    edge = [px[x, y0] for x in range(x0 + 8, x1 - 8)]
    bg = sum(1 for c in edge if near(c, PANEL, 40))
    report(bg == 0, "panel top edge stroked", f"({bg} unstroked px)")
    corners = [(x0, y0), (x1 - 1, y0), (x0, y1 - 1), (x1 - 1, y1 - 1)]
    cut = sum(1 for c in corners
              if near(px[c], PANEL, 40) or not greyish(px[c]))
    report(cut == 4, "panel corners cut by the radius", f"({cut}/4)")

    # ---- not blank, not flat ----
    cols = {px[x, y] for y in range(0, H, 3) for x in range(0, W, 3)}
    report(len(cols) > 50, "scene rendered", f"({len(cols)} distinct colours)")

    print("DEVICE CHECK PASSED" if fail == 0 else f"DEVICE CHECK FAILED ({fail})")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
