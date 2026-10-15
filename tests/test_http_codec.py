"""Validate actual incremental codec under ASan/UBSan; no protocol mock."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]


class HttpCodecTests(unittest.TestCase):
    def test_fragmented_protocol_and_bounds(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = str(pathlib.Path(folder) / 'http-codec')
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                '-I' + str(ROOT / 'systems/zephyr/services'),
                str(ROOT / 'tests/http_codec_host.c'),
                str(ROOT / 'systems/zephyr/services/aster_http_codec.c'),
                '-o', binary], check=True, capture_output=True, text=True)
            result = subprocess.run([binary], check=True, capture_output=True,
                                    text=True, timeout=20)
            self.assertIn('boundaries OK', result.stdout)
