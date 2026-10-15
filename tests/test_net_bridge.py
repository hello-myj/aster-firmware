"""Compile actual native bridge + transport, exercising hostile guest arguments."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]

class NetworkBridge(unittest.TestCase):
    def test_native_ownership_bounds_and_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = str(pathlib.Path(directory) / 'bridge')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                '-I'+str(ROOT/'tests'), '-I'+str(ROOT/'contract/include'),
                '-I'+str(ROOT/'systems/zephyr/core'), '-I'+str(ROOT/'systems/zephyr/services'),
                str(ROOT/'tests/net_bridge_host.c'), '-o', exe], check=True)
            subprocess.run([exe], check=True, timeout=20)
