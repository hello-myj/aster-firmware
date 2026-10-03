#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rasterize the licensed FLUX SVG chrome at its base and ribbon sizes.
Requires cairosvg and Pillow. SVG license: launcher/flux-assets/LICENSE.
"""
from pathlib import Path
from io import BytesIO
import cairosvg
from PIL import Image
ROOT=Path(__file__).resolve().parents[1]/'systems/zephyr/ui/launcher'
NAMES=['settings-2','chevron-left','chevron-right','clock','folder-open','terminal','bluetooth','moon','wifi','move','battery-full','box','arrow','aster']

def main():
    result=['/* Lucide icons from FLUX Rev.04. ISC license: see flux-assets/LICENSE. */']
    for size in [24,40]:
        for i,name in enumerate(NAMES):
            svg=(ROOT/'flux-assets/icons'/f'{name}.svg').read_bytes()
            bitmap=Image.open(BytesIO(cairosvg.svg2png(bytestring=svg,output_width=size*4,output_height=size*4))).convert('RGBA').getchannel('A').resize((size,size),Image.Resampling.LANCZOS).tobytes()
            result.append(f'static const uint8_t flux_icon_{size}_{i}[] = {{')
            result.extend(','.join(map(str,bitmap[p:p+size]))+',' for p in range(0,len(bitmap),size))
            result.append('};')
        result.append('static const uint8_t *const flux_icon_data'+('' if size==24 else '_40')+'[] = {'+','.join(f'flux_icon_{size}_{i}' for i in range(len(NAMES)))+'};')
    (ROOT/'flux_icons.inc').write_text('\n'.join(result)+'\n')

if __name__=='__main__': main()
