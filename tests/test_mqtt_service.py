"""Actual bounded MQTT/HTTP/TCP service with real loopback and wire faults."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class MqttServiceTests(unittest.TestCase):
    def run_transport(self, tls=False):
        with tempfile.TemporaryDirectory() as folder:
            binary = str(pathlib.Path(folder) / 'mqtt-service')
            build = subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                *(['-DASTER_MQTT_TLS_TEST=1'] if tls else []),
                '-I' + str(ROOT / 'tests'),
                '-I' + str(ROOT / 'systems/zephyr/core'),
                '-I' + str(ROOT / 'systems/zephyr/services'),
                str(ROOT / 'tests/mqtt_service_host.c'),
                str(ROOT / 'systems/zephyr/services/aster_http_codec.c'),
                str(ROOT / 'systems/zephyr/services/aster_mqtt_codec.c'),
                '-o', binary], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('100 lifecycle recovery OK', result.stdout)

    def test_mqtt_transport(self):
        self.run_transport()

    def test_tls_shared_admission(self):
        self.run_transport(tls=True)
