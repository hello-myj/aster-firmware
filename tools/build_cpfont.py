#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Bake cpfont v4 bitmap fonts (CrossPoint-compatible layout) from ui.ttf.
#
# Format spec + provenance: docs/cpfont-notes.md (mechanism ported from
# CrossPoint Reader CJK, MIT; generator written independently against the
# on-disk layout of lib/EpdFont/scripts/fontconvert_sdcard.py).
#
#   python3 tools/build_cpfont.py [--ttf PATH] [--sizes 12,14,...]
#                                    [--ss 4] [--gamma 1.0] [--out DIR]
#
# Rasterisation: render at ss x ppem, box-downsample to px with a real sample
# count at the edges, crop to the ink box, then quantise through a table that
# rounds (the old v >> 4 crush deleted levels 0..15, which is where small CJK
# hairlines live). Advances and line metrics always come from a separate 1x
# pass -- see rasterize().
#
# Output: resources/fonts/ui_<px>.cpfont  (single style: regular)
#         resources/fonts/faces.json     (bake record: sizes + metrics;
#                                          font_metrics.py turns it into the
#                                          firmware/SDK tables)
#   * 12/14/16/18/20/24 : every codepoint mapped in ui.ttf (~29k)
#   * 32/48             : Latin + CJK punct + kana + GB2312 level-1 subset
#                         (clock / big titles; keeps the big files small)
# Sizes are DEVICE PIXELS (set_char_size y_size = px<<6 @ 72 dpi), unlike
# CrossPoint's own script which uses points @150 dpi.

import argparse
import json
import math
import struct
import subprocess
import sys
from pathlib import Path

import freetype
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "resources" / "fonts"
TTF_DEFAULT = FONT_DIR / "ui.ttf"
MANIFEST_DEFAULT = FONT_DIR / "faces.json"

# Sizes baked when --sizes is not given. This is the ONLY place the set is
# decided: the bake writes every number the consumers need into
# faces.json, and tools/font_metrics.py turns that into the firmware/SDK
# tables (contract/include/aster/font_metrics.h) and checks them.
# Nothing downstream keeps its own copy of the list.
FULL_SIZES = [12, 14, 16, 18, 20, 24]
BIG_SIZES = [32, 48]

MAGIC = b"CPFONT\x00\x00"
CPFONT_VERSION = 4
FLAGS_4BIT = 0		# format v4 flags bit0: 1 = 2bpp, 0 = 4bpp (CrossPoint
			# semantics). ASTER ships 4bpp: 16 levels make small CJK
			# strokes read as crisp as the tiny_ttf(A8) fallback;
			# cost is storage + ring bytes, not the resident tables.
			# Anything else in this byte is unencodable, which is why
			# font_sd.c rejects a file whose hdr[10] has other bits set
			# instead of reading it as 4bpp garbage.
HEADER_SIZE = 32
STYLE_TOC_SIZE = 32
GLYPH_FMT = "<BBHhhH2xI"
assert struct.calcsize(GLYPH_FMT) == 16
TOC_FMT = "<B3xIIBhhHHBBBI4x"
assert struct.calcsize(TOC_FMT) == STYLE_TOC_SIZE

# Supersample factor for the rasteriser. 4 is where a 20 px CJK glyph's
# hairlines stop depending on which pixel the stem happened to land on; 8
# doubles the bake for no visible change at these sizes.
SS_DEFAULT = 4
# Applied to grey BEFORE quantisation. 1.0 is the calibrated default; the
# panels are dark-background/light-text and a value around 1.25 lifts the
# midtones if that reads too thin on glass.
GAMMA_DEFAULT = 1.0
DEPTH_DEFAULT = 4


def make_lut(gamma, depth=DEPTH_DEFAULT):
    """256-entry grey -> `depth`-bit level table.

    Two things changed from the old `v >> 4` crush. The quantiser ROUNDS:
    truncation maps 0..15 onto level 0, and 0..15 is exactly where a small
    CJK glyph's hairlines and antialiased edges live, so the old bake simply
    deleted them -- that, not the glyph shapes, is the "fuzzy" it was blamed
    for. And gamma is applied BEFORE quantisation, where the levels are still
    evenly spaced; applied after, it would re-band an already-banded signal.
    """
    lev = (1 << depth) - 1
    out = []
    for v in range(256):
        if gamma != 1.0:
            v = int(round(255.0 * (v / 255.0) ** gamma))
        out.append(min(lev, (v * lev + 127) // 255))
    return bytes(out)


def unpack_rows(bm):
    """A FreeType bitmap -> tight top-down rows of `width` bytes each."""
    w, rows, pitch = bm.width, bm.rows, bm.pitch
    buf = bytes(bm.buffer)
    stride = abs(pitch)
    if pitch >= 0:
        lv = b"".join(buf[r * stride:r * stride + w] for r in range(rows))
    else:                                 # bottom-up store
        lv = b"".join(buf[(rows - 1 - r) * stride:
                          (rows - 1 - r) * stride + w] for r in range(rows))
    return lv, w, rows


def resample_anchored(gray4, w4, h4, l4, t4, left1, top1, ss, margin=2):
    """Box-average an ss-x grey bitmap onto the DEVICE pixel grid, declaring
    the result to sit at (left, top) in the 1x frame. Returns
    (levels, w, h, left, top).

    The sampling phase is DERIVED FROM THE DECLARED ORIGIN (ox/oy below), and
    that is what makes placement exact: a glyph drawn from origin `left` is
    sampled so that its content lands on device column `left` and no other.
    Consequence worth knowing before "fixing" anything here: the choice of
    origin is free. FreeType hints each ppem independently, so the ss-x ink
    box sits on a different sub-pixel phase than the 1x one (top4/ss lands on
    .25/.50/.75 of a pixel), but shifting the origin by one device pixel
    shifts the block grid by exactly `ss` samples -- the same grid, one block
    over -- and the declared position moves with it. Declaring the 1x metrics
    and declaring round(top4/ss) produce byte-identical files; measured, not
    assumed. The origin only has to sit NEAR the ink, because `ow`/`oh` are
    sized from the ss-x bitmap plus a fixed margin and a wild origin would
    clip the far edge.

    Samples outside the ss-x bitmap are BACKGROUND, not missing data: the
    bitmap IS the ink bounding box, so coverage beyond it is zero by
    definition, and they are averaged in as zeroes. Dividing a partial edge
    block by however many samples happen to be inside instead would brighten
    the last column and row into a rim that is not in the glyph -- a
    one-column bitmap is the limiting case, and it must land at 1/ss**2 of
    its grey, not at full ink.

    Box rather than a sharpening kernel: at 4bpp a kernel with negative lobes
    rings, and ringing reads as noise, not as sharpness."""
    n = ss * ss
    left = left1 - margin
    top = top1 + margin
    ox = ss * left - l4                  # device-grid phase, in 4x samples
    oy = t4 - ss * top
    ow = 2 * margin + (w4 + ss - 1) // ss
    oh = 2 * margin + (h4 + ss - 1) // ss
    out = bytearray(ow * oh)
    for Y in range(oh):
        y0 = oy + ss * Y
        for X in range(ow):
            x0 = ox + ss * X
            acc = 0
            for y in range(y0, y0 + ss):
                if y < 0 or y >= h4:
                    continue
                row = y * w4
                for x in range(x0, x0 + ss):
                    if 0 <= x < w4:
                        acc += gray4[row + x]
            out[Y * ow + X] = (acc + n // 2) // n
    return bytes(out), ow, oh, left, top


def crop_ink(lv, w, h, left, top):
    """Drop whole all-zero rows and columns from a level array, adjusting
    `left`/`top` by the same whole pixels.

    Every surviving pixel therefore lands exactly where it did before: this
    is storage, not layout. It is also what keeps `dlen` small enough for the
    reader's per-glyph staging buffer (firmware: STG_BMP) to stay honest."""
    x0, x1, y0, y1 = w, -1, h, -1
    for y in range(h):
        row = lv[y * w:(y + 1) * w]
        if not any(row):
            continue
        if y < y0:
            y0 = y
        y1 = y
        for x in range(w):
            if row[x]:
                if x < x0:
                    x0 = x
                if x > x1:
                    x1 = x
    if x1 < 0:
        return b"", 0, 0, 0, 0        # nothing survived the quantiser
    nw, nh = x1 - x0 + 1, y1 - y0 + 1
    if x0 == 0 and y0 == 0 and nw == w and nh == h:
        return lv, w, h, left, top
    out = bytearray(nw * nh)
    for y in range(nh):
        s = (y + y0) * w + x0
        out[y * nw:(y + 1) * nw] = lv[s:s + nw]
    return bytes(out), nw, nh, left + x0, top - y0


def pack4(lv):
    """2 levels per byte, MSB first, whole glyph -- rows are NOT byte-padded
    (the reader indexes the packed array flat)."""
    lv = lv + bytes((-len(lv)) % 2)
    return bytes((lv[i] << 4) | lv[i + 1] for i in range(0, len(lv), 2))


def gb2312_level1():
    """GB2312 quwei rows 11+ (level-1 hanzi) decoded to codepoints."""
    cps = set()
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except Exception:
                continue
            if len(ch) == 1:
                cps.add(ord(ch))
    return cps


def big_subset(cps):
    """Latin/punct/kana/fullwidth + GB2312-1: what 32/48px really needs."""
    gb1 = gb2312_level1()
    keep = set()
    for cp in cps:
        if (cp < 0x0400 or 0x2000 <= cp <= 0x206F or
                0x3000 <= cp <= 0x303F or 0x3040 <= cp <= 0x30FF or
                0xFF00 <= cp <= 0xFF60 or cp in gb1):
            keep.add(cp)
    return keep


def to_intervals(cps):
    """Sorted unique codepoints -> CrossPoint intervals + U+FFFD. Returns
    merged (first, last, offset) list and the index-aligned codepoint order."""
    cps = sorted(set(cps) | {0xFFFD})
    intervals = []
    offset = 0
    run_lo = run_hi = cps[0]
    for cp in cps[1:]:
        if cp == run_hi + 1:
            run_hi = cp
            continue
        intervals.append((run_lo, run_hi, offset))
        offset += run_hi - run_lo + 1
        run_lo = run_hi = cp
    intervals.append((run_lo, run_hi, offset))
    return intervals, cps


def glyph_index(intervals, cp):
    lo, hi = 0, len(intervals) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        f, l, off = intervals[mid]
        if cp < f:
            hi = mid - 1
        elif cp > l:
            lo = mid + 1
        else:
            return off + (cp - f)
    return -1


def rasterize(ttf_path, px, cps, lut, ss=SS_DEFAULT):
    """Render every cp in `cps` at px device pixels. Returns
    (intervals, order, glyphs, bitmaps, advanceY, ascender, descender, ext)
    with glyphs/bitmaps index-aligned to `order` (== to_intervals order).

    Two passes over `order`, on purpose. Pass 1 runs at 1x and is the ONLY
    source of `adv` and of advanceY/ascender/descender: `linearHoriAdvance`
    is in pixels at the current char size, so reading it off the ss-x pass
    and dividing back would shift every advance by up to 1/16 px -- and every
    layout in the firmware is built on those advances. Pass 2 runs at ss-x
    and supplies pixels only.

    FT_LOAD_RENDER is kept at both sizes and no FT_LOAD_NO_HINTING / TARGET_*
    is added: hinted stems are what the box filter averages into a crisp
    vertical, and 4x ppem is high enough that hinting is stable there."""
    face = freetype.Face(str(ttf_path))
    order = to_intervals(cps)[1]

    face.set_char_size(0, px << 6, 72, 72)   # y_size px -> ppem == px
    advanceY = int(math.ceil(face.size.height / 64))
    ascender = int(math.ceil(face.size.ascender / 64))
    descender = int(math.floor(face.size.descender / 64))

    meta = []
    for cp in order:
        gi = face.get_char_index(cp)
        if gi == 0 and cp != 0xFFFD:
            gi = face.get_char_index(0xFFFD)  # replacement stands in
        if gi == 0:
            meta.append((0, 0, 0, False))
            continue
        face.load_glyph(gi, freetype.FT_LOAD_RENDER)
        g = face.glyph
        adv = (g.linearHoriAdvance + (1 << 11)) >> 12
        meta.append((adv, g.bitmap_left, g.bitmap_top,
                     bool(g.bitmap.width and g.bitmap.rows)))

    if ss > 1:
        face.set_char_size(0, (px * ss) << 6, 72, 72)

    glyphs = []
    bitmaps = []
    total_bitmap = 0
    max_bmp = 0
    max_scratch = 0
    for idx, cp in enumerate(order):
        if idx and idx % 4000 == 0:
            print(f"  {px}px: {idx}/{len(order)}", file=sys.stderr, flush=True)
        adv, left1, top1, ink1 = meta[idx]
        gi = face.get_char_index(cp)
        if gi == 0:
            gi = face.get_char_index(0xFFFD)
        if gi == 0:
            glyphs.append((0, 0, 0, 0, 0, 0, total_bitmap))
            bitmaps.append(b"")
            continue
        face.load_glyph(gi, freetype.FT_LOAD_RENDER)
        g = face.glyph
        bm = g.bitmap
        if ss > 1:
            gray, w, h = unpack_rows(bm)
            if w == 0 or h == 0:
                glyphs.append((0, 0, adv, 0, 0, 0, total_bitmap))
                bitmaps.append(b"")
                continue
            # Either origin gives byte-identical output (see resample_anchored),
            # so this picks on robustness, not placement: when the 1x render
            # came out empty its bearings are (0,0), which is far enough from
            # the ss-x ink to push the far edge of the glyph out of the
            # fixed-size output array. Anchoring to a bearing we know is near
            # the ink keeps that edge inside.
            if ink1:
                al, at = left1, top1
            else:
                al, at = (int(round(g.bitmap_left / ss)),
                          int(round(g.bitmap_top / ss)))
            gray, w, h, left, top = resample_anchored(
                gray, w, h, g.bitmap_left, g.bitmap_top, al, at, ss)
        else:
            w, h, left, top = bm.width, bm.rows, left1, top1
            if w == 0 or h == 0:              # space etc: advance only
                glyphs.append((0, 0, adv, 0, 0, 0, total_bitmap))
                bitmaps.append(b"")
                continue
            gray, w, h = unpack_rows(bm)
        lv = gray.translate(lut)
        lv, w, h, left, top = crop_ink(lv, w, h, left, top)
        packed = pack4(lv) if w and h else b""
        if w and h:
            glyphs.append((w, h, adv, left, top, len(packed), total_bitmap))
            max_bmp = max(max_bmp, len(packed))
            max_scratch = max(max_scratch, w * h)
        else:                                 # quantised away: advance only
            glyphs.append((0, 0, adv, 0, 0, 0, total_bitmap))
        bitmaps.append(packed)
        total_bitmap += len(packed)

    intervals = to_intervals(cps)[0]
    ext = {"max_bmp": max_bmp, "max_scratch": max_scratch}
    return (intervals, order, glyphs, bitmaps,
            advanceY, ascender, descender, ext)


def write_cpfont(path, intervals, order, glyphs, bitmaps,
                 advanceY, ascender, descender, flags=FLAGS_4BIT):
    """Single-style (regular) v4 file."""
    assert glyph_index(intervals, order[0]) == 0
    assert len(glyphs) == len(order) == len(bitmaps)
    intervals_blob = b"".join(struct.pack("<III", f, l, o)
                              for f, l, o in intervals)
    glyphs_blob = b"".join(struct.pack(GLYPH_FMT, w, h, adv, left, top, dlen,
                                        doff)
                          for w, h, adv, left, top, dlen, doff in glyphs)
    bitmap_blob = b"".join(bitmaps)
    data_off = HEADER_SIZE + STYLE_TOC_SIZE
    toc = struct.pack(TOC_FMT, 0, len(intervals), len(glyphs), advanceY,
                      ascender, descender, 0, 0, 0, 0, 0, data_off)
    header = struct.pack("<8sHHB19s", MAGIC, CPFONT_VERSION, flags, 1,
                         bytes(19))
    assert len(header) == HEADER_SIZE and len(toc) == STYLE_TOC_SIZE
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "wb") as f:
        f.write(header + toc + intervals_blob + glyphs_blob + bitmap_blob)
    tmp.rename(path)
    return path.stat().st_size


def write_manifest(path, ttf, records, big_from, bake):
    """The bake record: one entry per .cpfont written, carrying the exact
    metrics from its header TOC plus what it cost.

    Downstream consumers (font_metrics.py -> font_sd.h / aster_text.h /
    aster_deploy.py) read this instead of keeping a size list of their own.
    Rewriting it is part of baking, so it is written atomically next to the
    fonts and only ever reflects a complete set."""
    doc = {
        "generator": "tools/build_cpfont.py",
        "format": "cpfont",
        "version": CPFONT_VERSION,
        "flags": FLAGS_4BIT,
        "ttf": Path(ttf).name,
        "big_from": big_from,
        # How this set was produced. Only the metrics drive the firmware
        # tables, but a .cpfont on a card can be traced back to a bake
        # recipe with this.
        "bake": bake,
        "faces": records,
        # Reader-side buffer bounds, derived from what was actually baked:
        # font_metrics.py turns them into ASTER_FONT_MAX_BMP / _MAX_SCRATCH and
        # font_sd.c BUILD_ASSERTs its own buffers against them. A glyph
        # larger than either buffer used to be dropped in silence.
        "max_bmp": max(f["max_bmp"] for f in records),
        "max_scratch": max(f["max_scratch"] for f in records),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n")
    tmp.rename(path)
    print(f"manifest -> {path.name} ({len(records)} faces)",
          file=sys.stderr)


def ensure_ttf(ttf_path):
    if ttf_path.is_file():
        return
    print(f"building {ttf_path.name} ...", file=sys.stderr)
    subprocess.run([sys.executable, str(ROOT / "tools" / "build_ui_ttf.py")],
                   check=True)
    if not ttf_path.is_file():
        sys.exit("build_ui_ttf.py did not produce " + str(ttf_path))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", default=str(TTF_DEFAULT))
    ap.add_argument("--sizes", default=None,
                    help="comma list; default 12,14,16,18,20,24,32,48")
    ap.add_argument("--big-from", type=int, default=32,
                    help="sizes >= this bake only the common subset")
    ap.add_argument("--out", default=str(FONT_DIR),
                    help="directory for the .cpfont files (and, unless "
                         "--manifest says otherwise, for faces.json)")
    ap.add_argument("--manifest", default=None,
                    help="manifest path; 'none' to skip writing it. "
                         "Defaults to <out>/faces.json, so an experimental "
                         "bake into a scratch dir leaves the real one alone")
    ap.add_argument("--ss", type=int, default=SS_DEFAULT,
                    help=f"supersample factor before box-downsampling "
                         f"(default {SS_DEFAULT}; 1 = render at 1x)")
    ap.add_argument("--gamma", type=float, default=GAMMA_DEFAULT,
                    help=f"grey gamma applied before quantisation "
                         f"(default {GAMMA_DEFAULT}). Applied as "
                         f"v**(gamma), so >1 DARKENS midtones -- thinner "
                         f"strokes, crisper, but hairlines can fall to level "
                         f"0; <1 fattens them. 1.0 is the bake's default "
                         f"because darkening partly undoes the rounding fix")
    ap.add_argument("--depth", type=int, default=DEPTH_DEFAULT,
                    help="bitmap depth. Only 4 is implemented: 2bpp would "
                         "halve storage (which is on the SD card, not the "
                         "resident budget) at half the levels, and 8bpp has "
                         "no encoding in the v4 header -- it would be a v5")
    ap.add_argument("--emit-limits", action="store_true",
                    help="bake, then print ASTER_FONT_MAX_BMP/_MAX_SCRATCH "
                         "and exit; the values font_metrics.py would generate")
    args = ap.parse_args(argv)

    if args.depth != 4:
        sys.exit(f"--depth {args.depth}: only 4bpp is implemented "
                 f"(see --help; 2bpp trades levels for SD storage and 8bpp "
                 f"needs a format version bump)")
    if args.ss < 1:
        sys.exit("--ss must be >= 1")

    out_dir = Path(args.out)
    man = args.manifest if args.manifest is not None else str(
        out_dir / MANIFEST_DEFAULT.name)
    manifest = None if man == "none" else Path(man)

    ttf = Path(args.ttf)
    ensure_ttf(ttf)
    cmap = TTFont(str(ttf), lazy=True).getBestCmap()
    all_cps = set(cmap)
    subset_cps = big_subset(all_cps)
    sizes = ([int(s) for s in args.sizes.split(",")] if args.sizes
             else FULL_SIZES + BIG_SIZES)

    lut = make_lut(args.gamma, args.depth)
    print(f"bake: {args.ss}x supersample, gamma {args.gamma}, "
          f"{args.depth}bpp", file=sys.stderr)

    grand = 0
    records = []
    for px in sizes:
        cps = all_cps if px < args.big_from else subset_cps
        out = out_dir / f"ui_{px}.cpfont"
        print(f"ui_{px}.cpfont: rasterizing {len(cps)} glyphs ...",
              file=sys.stderr, flush=True)
        (intervals, order, glyphs, bitmaps,
         advY, asc, desc, ext) = rasterize(ttf, px, cps, lut, args.ss)
        size = write_cpfont(out, intervals, order, glyphs, bitmaps,
                            advY, asc, desc)
        grand += size
        records.append({
            "px": px,
            "file": out.name,
            "line_h": advY,
            "asc": asc,
            "desc": desc,
            "coverage": "full" if px < args.big_from else "gb2312",
            "intervals": len(intervals),
            "codepoints": len(order),
            "bytes": size,
            "max_bmp": ext["max_bmp"],
            "max_scratch": ext["max_scratch"],
        })
        print(f"  -> {out.name}: {len(intervals)} intervals, {len(glyphs)} "
              f"glyphs, advY={advY} asc={asc} desc={desc}, "
              f"max_bmp={ext['max_bmp']} max_scratch={ext['max_scratch']}, "
              f"{size / 1024 / 1024:.2f} MB", file=sys.stderr, flush=True)

    if args.emit_limits or manifest is not None:
        max_bmp = max(f["max_bmp"] for f in records)
        max_scr = max(f["max_scratch"] for f in records)
        if args.emit_limits:
            print(f"#define ASTER_FONT_MAX_BMP     {max_bmp}u")
            print(f"#define ASTER_FONT_MAX_SCRATCH {max_scr}u")
            print(f"total {grand / 1024 / 1024:.2f} MB", file=sys.stderr)
            return 0
        write_manifest(manifest, ttf, records, args.big_from,
                       {"ss": args.ss, "gamma": args.gamma,
                        "depth": args.depth, "lut": "round"})
    print(f"total {grand / 1024 / 1024:.2f} MB", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
