"""Actual HTTP/native bridge and transport: guest bounds, ownership, teardown."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class HttpBridge(unittest.TestCase):
    def test_guest_boundaries_and_lifecycle(self):
        with tempfile.TemporaryDirectory() as folder:
            exe = str(pathlib.Path(folder) / 'http-bridge')
            result = subprocess.run([
                'cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                '-I' + str(ROOT/'tests'), '-I' + str(ROOT/'contract/include'),
                '-I' + str(ROOT/'systems/zephyr/core'),
                '-I' + str(ROOT/'systems/zephyr/services'),
                str(ROOT/'tests/http_bridge_host.c'),
                str(ROOT/'systems/zephyr/services/aster_http_codec.c'),
                '-o', exe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([exe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
