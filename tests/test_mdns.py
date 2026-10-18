"""Actual mDNS wire codec under ASan/UBSan (N5.2 publisher, host gate)."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]

class MdnsTests(unittest.TestCase):
    def build_run(self, source):
        with tempfile.TemporaryDirectory() as folder:
            binary = str(pathlib.Path(folder) / 'mdns')
            args = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                    '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                    '-I' + str(ROOT / 'tests'),
                    '-I' + str(ROOT / 'systems/zephyr/core'),
                    '-I' + str(ROOT / 'systems/zephyr/services'),
                    '-I' + str(ROOT / 'contract/include'),
                    str(ROOT / 'tests' / source)]
            result = subprocess.run(args + ['-o', binary], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(' OK', result.stdout)

    def test_codec_bounds(self): self.build_run('mdns_host.c')
