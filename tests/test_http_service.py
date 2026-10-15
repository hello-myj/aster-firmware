"""Run the actual HTTP worker and TCP service against real loopback peers."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]


class HttpServiceTests(unittest.TestCase):
    def test_http_transport(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = str(pathlib.Path(folder) / 'http-service')
            build = subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                '-I' + str(ROOT / 'tests'),
                '-I' + str(ROOT / 'systems/zephyr/core'),
                '-I' + str(ROOT / 'systems/zephyr/services'),
                str(ROOT / 'tests/http_service_host.c'),
                str(ROOT / 'systems/zephyr/services/aster_http_codec.c'),
                '-o', binary], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('recovery OK', result.stdout)
