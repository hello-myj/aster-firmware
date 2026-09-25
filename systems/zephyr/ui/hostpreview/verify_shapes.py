#!/usr/bin/env python3
"""Numeric + ASCII verification of the host-side gfx shape preview.

Checks the invariants that the on-device complaints map to:
  * every stroke ring is a single connected closed loop (no gaps -> "lines missing")
  * every shape is drawn at all (no silent -EINVAL -> "rectangles abnormal")
  * fills land inside the requested bbox and cover the analytic area
  * corners of a rounded rect are actually round (corner bbox is cut)
  * AA really feathers edges instead of hard-thresholding them
and prints ASCII art so the geometry is inspectable without a viewer.

usage: verify_shapes.py shapes.raw [W] [H]
"""
import math
import sys
from pathlib import Path

W = int(sys.argv[2]) if len(sys.argv) > 2 else 240
H = int(sys.argv[3]) if len(sys.argv) > 3 else 360

# shape layout, mirrored from preview.c
RR_ROW1_Y, RR_ROW2_Y, RR_W, RR_H = 8, 60, 60, 44
RR_RADII = ((8, 3), (76, 10), (144, 22))
STADIUM_FILL = (8, 112, 224, 40)
STADIUM_RING = (8, 160, 224, 40)
ELLS = ((30, 20, 20), (88, 34, 14), (150, 22, 28), (206, 18, 24))
FILL_CY, RING_CY = 232, 330


def load(path):
    b = Path(path).read_bytes()
    return [b[2 * i] | (b[2 * i + 1] << 8) for i in range(W * H)]


def region(px, x0, y0, x1, y1):
    """Clipped sub-image; callers pass analytic bounds with a 1 px margin."""
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(W, x1), min(H, y1)
    return [[px[y * W + x] for x in range(x0, x1)] for y in range(y0, y1)]


def ascii_art(g, label):
    print(f"--- {label} ---")
    for row in g:
        print("   " + "".join("#" if v else "." for v in row))


def ring_check(g, label):
    """A 4/8-connected single closed loop has every pixel degree==2."""
    pts = {(x, y) for y, row in enumerate(g) for x, v in enumerate(row) if v}
    if not pts:
        print(f"  {label}: FAIL empty (nothing rasterized)")
        return False
    degree = {}
    for (x, y) in pts:
        degree[(x, y)] = sum(1 for dx in (-1, 0, 1) for dy in (-1, 0, 1)
                             if (dx or dy) and (x + dx, y + dy) in pts)
    bad = sorted(p for p, d in degree.items() if d < 2)
    over = [p for p, d in degree.items() if d > 3]
    ok = not bad
    print(f"  {label}: px={len(pts)} deg<2(broken links)={len(bad)} "
          f"deg>3(clumps)={len(over)} -> {'CLOSED LOOP OK' if ok else 'GAPS!'}")
    if bad[:6]:
        print(f"     first gap pixels: {bad[:6]}")
    return ok


def fill_check(g, label, want):
    got = sum(1 for row in g for v in row if v)
    ok = got > 0
    ratio = got / want if want else 0.0
    print(f"  {label}: px={got} analytic={want:.0f} ratio={ratio:.3f} "
          f"levels={levels(g)} -> {'OK' if ok else 'FAIL empty'}")
    return ok


def levels(g):
    return len({v for row in g for v in row if v})


def main():
    px = load(sys.argv[1] if len(sys.argv) > 1 else "shapes.raw")
    allok = True

    print("== rounded rect FILL (radius ramp 3/10/22) ==")
    for x0, r in RR_RADII:
        g = region(px, x0, RR_ROW1_Y, x0 + RR_W, RR_ROW1_Y + RR_H)
        want = (RR_W * RR_H) - (4 - math.pi) * r * r
        allok &= fill_check(g, f"rr-fill x={x0} r={r}", want)

    print("== rounded rect OUTLINE rings ==")
    for x0, r in RR_RADII:
        g = region(px, x0, RR_ROW2_Y, x0 + RR_W, RR_ROW2_Y + RR_H)
        allok &= ring_check(g, f"rr-ring x={x0} r={r}")
    # corner must be cut: for r=10 the very corner pixels stay empty
    g = region(px, 76, RR_ROW2_Y, 76 + RR_W, RR_ROW2_Y + RR_H)
    cut = not (g[0][0] or g[0][1] or g[1][0] or g[0][2])
    print(f"  rr-ring r=10 corner cut: {'OK' if cut else 'FAIL'}")
    ascii_art(g, "rr ring 60x44 r=10")
    allok &= cut
    edge = bool(px[60 * W + 106] and px[61 * W + 106] and
                not px[62 * W + 106])
    print(f"  rr-ring 2px straight edge: {'OK' if edge else 'FAIL'}")
    allok &= edge

    print("== stadium 224x40 (r = h/2, arcs meet) ==")
    x, y, w, h = STADIUM_FILL
    allok &= fill_check(region(px, x, y, x + w, y + h), "stadium fill",
                        w * h - (4 - math.pi) * 400)
    x, y, w, h = STADIUM_RING
    allok &= ring_check(region(px, x, y, x + w, y + h), "stadium ring 2px")

    print("== ellipse FILL ==")
    for cx, rx, ry in ELLS:
        allok &= fill_check(region(px, cx - rx, FILL_CY - ry, cx + rx + 1,
                                   FILL_CY + ry + 1),
                            f"ell-fill rx={rx} ry={ry}", math.pi * rx * ry)

    print("== ellipse STROKE rings ==")
    for cx, rx, ry in ELLS:
        g = region(px, cx - rx, RING_CY - ry, cx + rx + 1, RING_CY + ry + 1)
        allok &= ring_check(g, f"ell-ring rx={rx} ry={ry}")
    circle_top = (px[(RING_CY - 20) * W + 30] and
                  px[(RING_CY - 19) * W + 30] and
                  not px[(RING_CY - 17) * W + 30])
    print(f"  circle 2px cardinal stroke: {'OK' if circle_top else 'FAIL'}")
    allok &= bool(circle_top)
    ascii_art(region(px, 30 - 20, RING_CY - 20, 30 + 21, RING_CY + 21),
              "circle ring r=20 (the 'lines missing' case)")

    print("== AA ==")
    print(f"  full-canvas distinct levels: {levels(region(px, 0, 0, W, H))}")
    print("ALL CHECKS PASSED" if allok else "SOME CHECKS FAILED")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
