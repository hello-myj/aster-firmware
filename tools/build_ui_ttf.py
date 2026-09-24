#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Build the single UI font for ABI 2.1: assets/fonts/ui.ttf.
#
# DroidSansFallbackFull is a FALLBACK font: its cmap carries CJK + a PUA
# icon block but NO ASCII letters/digits at all (only U+0020). Shipping it
# alone as ui.ttf renders Chinese fine and English/digits as tofu. So we
# graft DejaVu Sans's Latin coverage onto the Droid base, rescaled from
# DejaVu's 2048 unitsPerEm to Droid's 256. One file, one lv_tiny_ttf pool.
#
#   python3 tools/build_ui_ttf.py [--droid PATH] [--dejavu PATH]
#
# Output: resources/fonts/ui.ttf -- an intermediate only. build_cpfont.py bakes
# it into the on-card ui_<px>.cpfont faces (resources/fonts/NOTICE).

import argparse
import copy
import shutil
import sys
from pathlib import Path

from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "resources" / "fonts" / "ui.ttf"

# Fill every Latin/Droid-missing codepoint in this span from DejaVu. Covers
# printable ASCII (0x21-0x7E), Latin-1 (0xA0-0xFF) and the general Latin
# block; Droid's own mappings always win.
SCAN_LO, SCAN_HI = 0x20, 0x2FF


def find_font(cands):
    for c in cands:
        p = Path(c)
        if p.is_file():
            return p
    return None


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--droid", default=str(ROOT / "resources" / "fonts" /
                                           "DroidSansFallbackFull.ttf"))
    ap.add_argument("--dejavu", default=None)
    args = ap.parse_args(argv)

    dejavu = args.dejavu or find_font([
        ROOT / "resources" / "fonts" / "DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ])
    if not dejavu or not Path(args.droid).is_file():
        sys.exit("need both DroidSansFallbackFull.ttf and DejaVuSans.ttf")

    droid = TTFont(args.droid)
    dv = TTFont(dejavu)
    scale = droid["head"].unitsPerEm / dv["head"].unitsPerEm  # 256/2048
    glyf, hmtx = droid["glyf"], droid["hmtx"]
    cmap = droid.getBestCmap()          # codepoint -> glyph name
    dv_cmap, dv_glyf, dv_hmtx = dv.getBestCmap(), dv["glyf"], dv["hmtx"]

    added, skipped = 0, 0
    for cp in range(SCAN_LO, SCAN_HI + 1):
        if cp in cmap:                  # Droid covers it: keep its own
            continue
        src = dv_cmap.get(cp)
        if not src:
            skipped += 1
            continue
        g = dv_glyf[src]
        if g.numberOfContours > 0:      # simple glyphs only (all Latin fits)
            ng = copy.deepcopy(g)
            for i, (x, y) in enumerate(ng.coordinates):
                ng.coordinates[i] = (round(x * scale), round(y * scale))
        elif g.numberOfContours == 0:
            ng = copy.deepcopy(g)       # empty (e.g. no-break space)
        else:
            skipped += 1                # composite: not needed for our range
            continue
        name = f"dv{cp:04X}"
        glyf[name] = ng
        adv, lsb = dv_hmtx[src]
        hmtx[name] = (max(1, round(adv * scale)), round(lsb * scale))
        cmap[cp] = name
        added += 1

    for t in droid["cmap"].tables:      # both fmt4 (BMP) and fmt12 copies
        t.cmap.update({cp: cmap[cp] for cp in range(SCAN_LO, SCAN_HI + 1)
                       if cp in cmap and cmap[cp].startswith("dv")})
    droid["maxp"].numGlyphs = len(glyf.keys())
    droid["OS/2"].usFirstCharIndex = min(cmap)
    droid["OS/2"].usLastCharIndex = min(max(cmap), 0xFFFF)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    droid.save(OUT)

    chk = TTFont(OUT)
    c2 = chk.getBestCmap()
    missing = [chr(c) for c in range(0x21, 0x7F) if c not in c2]
    assert not missing, f"still missing: {missing}"
    print(f"ui.ttf: +{added} DejaVu glyphs (rescale x{scale:g}), "
          f"{skipped} skipped, {len(c2)} mapped cps, "
          f"{OUT.stat().st_size} bytes -> {OUT}")


if __name__ == "__main__":
    sys.exit(main())
