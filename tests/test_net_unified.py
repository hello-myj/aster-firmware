"""ABI 4 unified network import: handles, rejection, cancel visibility."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class UnifiedNetBridge(unittest.TestCase):
    def test_public_handles(self):
        with tempfile.TemporaryDirectory() as folder:
            exe = str(pathlib.Path(folder) / 'net-unified')
            build = subprocess.run([
                'cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                '-I' + str(ROOT / 'tests'), '-I' + str(ROOT / 'contract/include'),
                '-I' + str(ROOT / 'systems/zephyr/core'),
                '-I' + str(ROOT / 'systems/zephyr/services'),
                str(ROOT / 'tests/net_unified_bridge_host.c'), '-o', exe],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([exe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
