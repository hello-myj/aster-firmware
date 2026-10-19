"""N5.5 network state event ring and stage diagnostics under ASan/UBSan."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]

class NetEventTests(unittest.TestCase):
    def build_run(self, source):
        with tempfile.TemporaryDirectory() as folder:
            binary = str(pathlib.Path(folder) / 'net_event')
            args = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                    '-Wall', '-Wextra', '-Werror',
                    # The shared host platform header defines net/dns/thread
                    # stubs this focused test never calls; the service path that
                    # uses them is covered by the cross-compile and the net/http
                    # service fixtures instead.
                    '-Wno-unused-function',
                    '-g', '-fsanitize=address,undefined',
                    '-I' + str(ROOT / 'tests'),
                    '-I' + str(ROOT / 'systems/zephyr/core'),
                    '-I' + str(ROOT / 'systems/zephyr/services'),
                    str(ROOT / 'tests' / source)]
            result = subprocess.run(args + ['-o', binary], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(' OK', result.stdout)

    def test_event_ring_and_diagnostics(self): self.build_run('net_event_host.c')