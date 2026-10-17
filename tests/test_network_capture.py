"""UART validation must wait for command completion despite async APP prompts."""
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch

path = Path(__file__).resolve().parents[1] / 'tools/validation/network_http_capture.py'
spec = importlib.util.spec_from_file_location('network_capture_under_test', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Clock:
    value = 0
    def monotonic(self):
        return self.value
    def sleep(self, duration):
        self.value += duration


class Serial:
    def __init__(self, clock, events):
        self.clock, self.events = clock, iter(events)
        self.reads = 0
    def write(self, data):
        return len(data)
    def flush(self):
        pass
    def reset_input_buffer(self):
        pass
    def read(self, count):
        self.reads += 1
        self.clock.value += .1
        return next(self.events, b'')


class NetworkCapture(unittest.TestCase):
    def run_command(self, events, command):
        clock = Clock()
        serial = Serial(clock, events)
        with patch.object(module, 'time', clock):
            result = module.Client(serial, io.StringIO()).command(command)
        return result, serial.reads

    def test_async_prompt_before_echo_does_not_complete_touch_command(self):
        result, reads = self.run_command([
            b'APP heartbeat\r\naster:~$ hw touch drag 1 2 3 4 8 30\r\n',
            b'', b'', b'',
            b'injected touch drag 1,2 -> 3,4\r\naster:~$ ',
            b'', b'',
        ], 'hw touch drag 1 2 3 4 8 30')
        self.assertIn('injected touch drag', result)
        self.assertGreaterEqual(reads, 7)

    def test_redrawn_edit_line_is_not_an_idle_prompt(self):
        result, reads = self.run_command([
            b'aster:~$ hw touch tap 1 2\r\nAPP heartbeat\r\n',
            b'aster:~$ hw touch tap 1 2', b'', b'', b'',
            b'\r\ninjected touch tap x=1 y=2\r\naster:~$ ', b'', b'',
        ], 'hw touch tap 1 2')
        self.assertIn('injected touch tap', result)
        self.assertGreaterEqual(reads, 8)

    def test_network_command_requires_response_and_subsequent_prompt(self):
        result, _ = self.run_command([
            b'aster:~$ aster net http stats\r\n', b'', b'',
            b'HTTP active=0 closing=0\r\n', b'', b'',
            b'aster:~$ ', b'', b'',
        ], 'aster net http stats')
        self.assertIn('HTTP active=0 closing=0', result)

    def test_websocket_command_requires_ws_response(self):
        result, _ = self.run_command([
            b'aster:~$ aster net ws stats\r\n', b'',
            b'WS active=0 closing=0\r\naster:~$ ', b'', b'',
        ], 'aster net ws stats')
        self.assertIn('WS active=0 closing=0', result)

    def test_missing_completion_prompt_times_out(self):
        clock = Clock()
        serial = Serial(clock, [b'aster:~$ hw touch tap 1 2\r\n'])
        with patch.object(module, 'time', clock):
            with self.assertRaisesRegex(RuntimeError, 'serial timeout'):
                module.Client(serial, io.StringIO()).command('hw touch tap 1 2')


if __name__ == '__main__':
    unittest.main()
