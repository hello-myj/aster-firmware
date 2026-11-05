"""Run the actual HTTP worker and TCP service against real loopback peers."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]


class HttpServiceTests(unittest.TestCase):
    def run_transport(self, p4=False):
        with tempfile.TemporaryDirectory() as folder:
            binary = str(pathlib.Path(folder) / 'http-service')
            build = subprocess.run(['cc', *(['-DASTER_SYSTEM_ESP_IDF=1'] if p4 else []), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                '-I' + str(ROOT / 'tests'),
                '-I' + str(ROOT / 'systems/zephyr/core'),
                '-I' + str(ROOT / 'systems/zephyr/services'), '-I'+str(ROOT/'common/include'), '-I'+str(ROOT/'common/package'), '-I'+str(ROOT/'common/net'),
                str(ROOT / 'tests/http_service_host.c'),
                str(ROOT / 'common/net/aster_http_codec.c'),
                '-o', binary], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('recovery OK', result.stdout)

    def test_http_transport(self):
        self.run_transport()

    def test_p4_large_buffers_and_cleanup(self):
        self.run_transport(p4=True)
