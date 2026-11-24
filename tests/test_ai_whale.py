"""The wake view needs a full RGB565 color plane and a separate opacity plane."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class WakeImage(unittest.TestCase):
    def test_launcher_descriptor_matches_generated_planes(self):
        ui = (ROOT / "systems/zephyr/ui/launcher/ai_companion.inc").read_text(encoding="utf-8")
        start = ui.index("enum { AI_WHALE_WIDTH")
        end = ui.index("};", ui.index("static const lv_image_dsc_t ai_whale")) + 2
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            sprite = tmp / "whale.bin"
            subprocess.run([sys.executable, ROOT / "systems/espidf/optional/aster_ai/whale.py",
                            sprite], check=True)
            # Read the production descriptor through LVGL's own types and format constants.
            source = tmp / "layout.c"
            source.write_text('#include <stdio.h>\n#include "lvgl.h"\n'
                              'static const uint8_t ai_whale_pixels[1];\n' + ui[start:end] + '''
int main(void) {
    if (ai_whale.header.cf != LV_COLOR_FORMAT_RGB565A8) return 1;
    printf("%u %u %u %u", ai_whale.header.w, ai_whale.header.h,
           ai_whale.header.stride, (unsigned)ai_whale.data_size);
    return 0;
}
''', encoding="utf-8")
            exe = tmp / "layout"
            subprocess.run(["cc", "-std=c11", "-DLV_CONF_SKIP", "-I", ROOT / "third_party/lvgl",
                            source, "-o", exe], check=True)
            width, height, stride, size = map(int, subprocess.check_output([exe]).split())
            data = sprite.read_bytes()
        self.assertEqual(stride, width * 2)
        self.assertEqual(len(data), size)
        alpha = data[stride * height:]
        self.assertEqual(len(alpha), width * height)
        self.assertEqual(alpha[0], 0)  # Transparent outside the silhouette.
        self.assertEqual(alpha[45 * width + 80], 255)
        self.assertEqual(struct.unpack_from("<H", data, 45 * stride + 80 * 2)[0], 0x545b)
