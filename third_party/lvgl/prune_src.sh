#!/usr/bin/env bash
# Rebuild the vendored LVGL tree from a pristine v9.5.0 checkout.
#
# Policy: KEEP every .h (the umbrella lvgl.h #includes many unconditionally,
# so a dropped header dangles the core .c that pull it in). Delete ONLY the
# .c we never compile: platform drivers, heavy optional libs, GPU/vendor draw
# backends, unused font binaries, debug/test modules. lv_conf.h turns the
# matching features off, so the orphan headers contribute zero symbols.
#
# Because headers are never removed, lvgl.h stays pristine -- no umbrella prune
# step is needed.
#
# Usage:  LVGL_ROOT=/path/to/lvgl-9.5.0 bash prune_src.sh
set -eu
ROOT=${LVGL_ROOT:-/tmp/lvgl-tags-v9.5.0}
V=$(cd "$(dirname "$0")" && pwd)
[ -f "$ROOT/lvgl.h" ] || { echo "no $ROOT/lvgl.h"; exit 1; }

rm -rf "$V/src"
cp -r "$ROOT/src" "$V/src"
cp "$ROOT/lvgl.h" "$ROOT/lv_version.h" "$V/" 2>/dev/null || cp "$ROOT/lv_version.h" "$V/"
[ -f "$ROOT/LICENCE.txt" ] && cp "$ROOT/LICENCE.txt" "$V/"
cd "$V/src"

# 1. platform drivers: we blit to our own surface via the MJX bridge.
find drivers -name "*.c" -delete

# 2. heavy optional libs (keep bin_decoder + rle: native image format).
for d in FT800-FT813 frogfs gltf gstreamer libjpeg_turbo libwebp svg \
         vg_lite_driver bmp lodepng libpng freetype tiny_ttf rlottie thorvg \
         ffmpeg qrcode barcode fsdrv tjpgd; do
	[ -d "libs/$d" ] && find "libs/$d" -name "*.c" -delete
done

# 3. GPU / vendor draw backends: keep sw + snapshot + convert.
for d in eve nanovg nema_gfx nxp opengles renesas sdl vg_lite dma2d espressif; do
	[ -d "draw/$d" ] && find "draw/$d" -name "*.c" -delete
done
rm -f draw/lv_draw_3d.c

# 4. fonts: keep engine + montserrat sizes lv_conf enables.
find font -maxdepth 1 -name "lv_font_montserrat_*.c" \
	! -name "lv_font_montserrat_12.c" ! -name "lv_font_montserrat_14.c" \
	! -name "lv_font_montserrat_16.c" ! -name "lv_font_montserrat_28.c" -delete
rm -f font/lv_font_source_han_sans_sc_*.c font/lv_font_unscii_*.c \
      font/lv_font_dejavu_16_persian_hebrew.c

# 5. debug-only / test modules.
find debugging others/file_explorer -name "*.c" -delete

# 6. drop everything that is neither .c nor .h: .cpp/.cc/.hpp/.ttf/.md and
#    bundled assets never enter the wasm compile set and are never #included
#    from a kept header. (Thorvg is .cpp; freetype ships a .ttf; etc.)
find . -type f ! -name "*.c" ! -name "*.h" -delete

# 7. Put the local patches back (see PATCHES.md). Steps 1-6 are a pure
#    regeneration, so without this a prune silently reverts them -- and the
#    build still succeeds, so nothing else would ever tell you.
#    Applied from the repo root because the patch carries repo-root paths.
#    --forward makes a re-run idempotent; the checker below is what decides.
cd "$V/../../.."
patch -p1 --forward < sdk/vendor/lvgl/lvgl-mjx.patch || true
python3 sdk/tools/check_lvgl_patch.py

echo "vendored src: $(find "$V/src" -name '*.c' | wc -l) .c, $(du -sh "$V/src" | cut -f1)"
