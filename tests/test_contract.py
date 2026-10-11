import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from export_contract import bundle_files
from font_metrics import render


class Contract(unittest.TestCase):
    def test_public_headers_have_no_os_dependency(self):
        source = '#include "aster_platform.h"\n#include <aster/aster.h>\nint main(void) { return 0; }\n'
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["cc", "-std=c11", "-Werror", "-x", "c", "-", "-c", "-o", str(Path(directory) / "api.o"),
                            "-I" + str(ROOT / "platform"), "-I" + str(ROOT / "contract/include")],
                           input=source, text=True, check=True, capture_output=True)

    def test_contract_exports_no_build_details(self):
        files = bundle_files()
        for name, data in files.items():
            if name.startswith("targets/"):
                target = json.loads(data)
                self.assertNotIn("build", target)
                self.assertNotIn("overlay", target)
                self.assertNotIn("conf", target)
        self.assertGreater(len(json.loads(files["contract/api.json"])["signatures"]), 40)

    def test_release_is_reproducible(self):
        self.assertEqual(bundle_files(), bundle_files())

    def test_font_contract_matches_release_resources(self):
        self.assertEqual((ROOT / "contract/include/aster/font_metrics.h").read_text(), render())
        for face in json.loads((ROOT / "resources/fonts/faces.json").read_text())["faces"]:
            self.assertEqual((ROOT / "resources/fonts" / face["file"]).stat().st_size, face["bytes"])

    def test_hardware_exports_and_separate_selection(self):
        records = list((ROOT / "hardware").glob("*.json"))
        self.assertEqual({p.stem for p in records}, {"lckfb-s3", "waveshare-35b"})
        for record in records:
            public = json.loads(record.read_text())["public"]
            self.assertEqual(public["id"], record.stem)
            self.assertEqual(set(public["display"]["modes"]), {"landscape", "portrait", "headless"})
        bundle = bundle_files()
        self.assertEqual({name for name in bundle if name.startswith("targets/")},
                         {"targets/lckfb-s3.json", "targets/waveshare-35b.json"})
        import firmware
        selected = ROOT / "boards/waveshare_35b/selection.conf"
        config = firmware.load_kconfig(selected)
        self.assertEqual(config.named_choices["ASTER_HARDWARE_SELECT"].selection.name,
                         "ASTER_HW_WAVESHARE_35B")
        self.assertEqual(firmware.hardware(selected)["public"]["display"]["height"], 480)
        self.assertEqual(firmware.hardware(ROOT / "config/default.conf")["public"]["id"], "lckfb-s3")

    def test_menuconfig_modes_preserve_hardware(self):
        import firmware
        defaults = firmware.load_kconfig()
        defaults.load_config(str(ROOT / "config/default.conf"))
        self.assertEqual(defaults.named_choices["ASTER_BOOT_DISPLAY"].selection.name,
                         "ASTER_BOOT_PORTRAIT")
        self.assertNotIn("ASTER_BOOT_LANDSCAPE", defaults.syms)
        for mode in ("PORTRAIT", "HEADLESS"):
            config = firmware.load_kconfig(ROOT / "config/default.conf")
            config.syms["ASTER_BOOT_" + mode].set_value("y")
            self.assertEqual(config.named_choices["ASTER_BOOT_DISPLAY"].selection.name, "ASTER_BOOT_" + mode)
            self.assertEqual(config.named_choices["ASTER_HARDWARE_SELECT"].selection.name, "ASTER_HW_LCKFB_S3")
            self.assertEqual(config.syms["ASTER_SYSTEM_ZEPHYR"].str_value, "y")
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "aster.conf"
                config.write_config(str(path))
                config.load_config(str(path))
                self.assertEqual(config.named_choices["ASTER_BOOT_DISPLAY"].selection.name, "ASTER_BOOT_" + mode)

    def test_firmware_cli_has_no_target_argument(self):
        result = subprocess.run([sys.executable, str(ROOT / "tools/firmware.py"), "build", "--target", "lckfb-s3"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unrecognized arguments", result.stderr)

    def test_core_lifecycle_fixture(self):
        executable = ROOT / "build/host/bin/aster-host"
        if not executable.exists():
            self.skipTest("run tools/host.py first to run lifecycle integration")
        result = subprocess.run([str(executable), str(ROOT / "tests/fixtures/hello.wasm")],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("application lifecycle passed", result.stdout)
        with tempfile.TemporaryDirectory() as directory:
            invalid = Path(directory) / "invalid.wasm"
            invalid.write_bytes(b"invalid module data")
            result = subprocess.run([str(executable), str(invalid)], capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
