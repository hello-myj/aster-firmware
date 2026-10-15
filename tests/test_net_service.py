"""Exercise actual service C against POSIX loopback with deterministic DNS/clock."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]


class NetServiceTests(unittest.TestCase):
    def test_transport_and_lifecycle(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = str(pathlib.Path(temp) / 'net-service')
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'tests'),
                '-I' + str(ROOT / 'systems/zephyr/core'),
                str(ROOT / 'tests/net_service_host.c'), '-o', binary,
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([binary], check=True, capture_output=True,
                                    text=True, timeout=20)
            self.assertIn('recovery OK', result.stdout)
