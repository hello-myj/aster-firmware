"""Actual RFC6455 codec and shared services under ASan/UBSan."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]

class WebSocketTests(unittest.TestCase):
    def build_run(self, source, tls=False):
        with tempfile.TemporaryDirectory() as folder:
            binary=str(pathlib.Path(folder)/'websocket')
            args=['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined']
            if tls:args+=['-DASTER_WS_TLS_TEST=1']
            args+=['-I'+str(ROOT/'tests'),'-I'+str(ROOT/'systems/zephyr/core'),'-I'+str(ROOT/'systems/zephyr/services'),str(ROOT/'tests'/source),str(ROOT/'systems/zephyr/services/aster_http_codec.c')]
            if source=='ws_codec_host.c':args+=[str(ROOT/'systems/zephyr/services/aster_ws_codec.c')]
            result=subprocess.run(args+['-o',binary],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([binary],capture_output=True,text=True,timeout=45)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn(' OK',result.stdout)
    def test_codec_boundaries(self):self.build_run('ws_codec_host.c')
    def test_shared_transport(self):self.build_run('ws_service_host.c')
    def test_shared_tls(self):self.build_run('ws_service_host.c',True)
