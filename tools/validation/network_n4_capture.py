#!/usr/bin/env python3
"""N4 exclusive-UART runner: continuous raw evidence, metrics and bounded gestures.

Requires the prepared SDK-only network-stress APP and diagnostic firmware.
Does not flash or clean up original APPs. Keeps the validation APP foreground
until a separate manual interaction check and explicit cleanup. Logs stay outside Git.
"""
import argparse
import codecs
import json
from pathlib import Path
import re
import sys
import time
from network_http_capture import Client

ANSI = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')
APP = 'aster.validation.networkstress'


class ObservedSerial:
    def __init__(self, connection, log):
        self.connection, self.log = connection, log
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')
        self.pending = ''
        self.rows = {}
        self.failure = None
        self.started = time.monotonic()
        self.armed = False
        self.fast_pass = False
        self.text = ''
        self.events = {k:0 for k in ('return: confirmation opened','return: cancelled','qpanel: open (over app)','qpanel: close')}

    def __getattr__(self, name):
        return getattr(self.connection, name)

    def read(self, count=1):
        raw = self.connection.read(count)
        value = self.decoder.decode(raw)
        self.log.write(value); self.log.flush()
        self.text = (self.text+value)[-40000:]
        self.pending += value
        lines = self.pending.split('\n'); self.pending = lines.pop()
        for line in lines:
            clean = ANSI.sub('', line)
            for event in self.events:
                if event in clean:
                    self.events[event] += 1
            if 'N4FAST PASS' in clean:
                self.fast_pass = True
            match = re.search(r'\b(CONCUR|CONCURIO|CONCURPH|N4NET|N4LAT|N4UDP|N4RECOVER) (.*)', clean)
            if match:
                row = {k: int(v) for k,v in re.findall(r'(\w+)=(\d+)', match[2])}
                row['host_seconds'] = round(time.monotonic()-self.started, 3)
                self.rows.setdefault(match[1], []).append(row)
                if self.armed:
                    for key in ('drawerr', 'imuerr', 'audioerr', 'reserr', 'stale', 'errors'):
                        if match[1] == 'N4UDP':
                            continue  # UDP duplicates/late echoes are separate observations.
                        if row.get(key, 0):
                            self.failure = clean
            if self.armed and ('N4NET FAIL' in clean or '*** FATAL' in clean or 'Guru Meditation' in clean or 'aster_app_init refused' in clean or ('app startup failed:' in clean and APP in clean)):
                self.failure = clean
        return raw

    def reset_input_buffer(self):
        # Preserve heartbeats/FAIL lines arriving between shell commands.
        while self.connection.in_waiting:
            self.read(self.connection.in_waiting)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--sdk-tools', type=Path, required=True)
    p.add_argument('--package', type=Path, required=True)
    p.add_argument('--ca', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--seconds', type=int, default=600)
    p.add_argument('--width', type=int, required=True)
    p.add_argument('--height', type=int, required=True)
    p.add_argument('--fast', action='store_true')
    p.add_argument('--gestures', action='store_true')
    a = p.parse_args()
    if not a.fast and a.seconds < 600:
        p.error('long concurrency gate requires at least 600 continuous host seconds')
    sys.path.insert(0, str(a.sdk_tools.resolve()))
    from aster_send import open_serial, send_file
    a.output.mkdir(parents=True, exist_ok=True)
    status = {'status': 'running', 'continuous_seconds_requested': a.seconds, 'fast': a.fast,
              'physical_display': 'pending_manual', 'physical_speaker': 'not_verified'}
    with open_serial(a.port, timeout=.1, write_timeout=10, exclusive=True) as connection, \
            (a.output/'serial.txt').open('w') as log, (a.output/'commands.txt').open('w') as commands:
        observed = ObservedSerial(connection, log)
        c = Client(observed, commands)
        def cmd(s):
            text = c.command(s)
            if observed.failure:
                raise AssertionError(observed.failure)
            return text
        def watch(seconds):
            end = time.monotonic()+seconds
            while time.monotonic() < end:
                observed.read(8192)
                if observed.failure:
                    raise AssertionError(observed.failure)
        def wait_event(event, before, timeout=5):
            deadline = time.monotonic()+timeout
            while observed.events[event] <= before and time.monotonic() < deadline:
                watch(.1)
            assert observed.events[event] > before, 'missing UI event: '+event
        def snapshot(label):
            print('SNAPSHOT', label, flush=True)
            for s in ['aster net mqtt stats', 'aster net http stats', 'aster net stats',
                      'aster sys mem', 'netpool', 'net mem', 'kernel thread stacks']:
                print(cmd(s), flush=True)
        try:
            for s in ['aster sys info', 'hw wifi status', 'hw audio vol']:
                print(cmd(s), flush=True)
            cmd('aster app stop '+APP)  # harmless when not currently foreground
            watch(2)
            assert send_file(a.port, a.ca, '/SD:/n4-ca.pem', connection=observed) == 'OK'
            assert 'HTTP result=0' in cmd('aster net http ca /SD:/n4-ca.pem')
            assert send_file(a.port, a.package, '/SD:/n4-stress.ast', connection=observed) == 'OK'
            result = cmd('aster app install /SD:/n4-stress.ast')
            assert 'installed OK' in result, result
            snapshot('before APP')
            cmd('netpool reset')
            observed.rows.clear()  # exclude the previous APP's stop report
            observed.armed = True
            print(cmd('aster app start '+APP), flush=True)
            assert 'RUNNING' in cmd('aster app state '+APP), 'validation APP did not start'
            observed.started = time.monotonic()
            start = time.monotonic(); last_snapshot = -1; gestures = 0
            while time.monotonic()-start < a.seconds:
                watch(.2)
                elapsed = time.monotonic()-start
                if a.fast and observed.fast_pass:
                    break
                if not a.fast:
                    minute = int(elapsed)//60
                    if minute != last_snapshot:
                        snapshot(f'minute {minute}')
                        last_snapshot = minute
                    if a.gestures and elapsed >= 30+gestures*65 and gestures < 9:
                        before = dict(observed.events)
                        # Each command drains incoming evidence; coordinates are panel-native.
                        x = a.width//2
                        print(cmd(f'hw touch drag {x} {a.height-4} {x} {a.height-100} 8 30'), flush=True)
                        wait_event('return: confirmation opened', before['return: confirmation opened'])
                        cw = min(a.width-32, 288)
                        cancel_x = (a.width-cw)//2+18+(cw-44)//4
                        cancel_y = (a.height-194)//2+159
                        print(cmd(f'hw touch tap {cancel_x} {cancel_y}'), flush=True)
                        wait_event('return: cancelled', before['return: cancelled'])
                        print(cmd(f'hw touch drag {x} 4 {x} 180 8 30'), flush=True)
                        wait_event('qpanel: open (over app)', before['qpanel: open (over app)'])
                        # The APP control sheet toggles with a second top-edge pull-down.
                        print(cmd(f'hw touch drag {x} 4 {x} 180 8 30'), flush=True)
                        wait_event('qpanel: close', before['qpanel: close']+1)
                        assert 'RUNNING' in cmd('aster app state '+APP)
                        gestures += 1
                        print('GESTURE_PASS', gestures, flush=True)
                if not connection.is_open:
                    raise RuntimeError('serial disconnected: the continuous run is invalid')
            elapsed = time.monotonic()-start
            if a.fast:
                assert observed.fast_pass, '100 completion/cancel gate did not finish'
                watch(2)
            else:
                assert elapsed >= 600
                assert 'RUNNING' in cmd('aster app state '+APP)
                assert observed.rows.get('N4NET') and observed.rows.get('CONCUR')
                last = observed.rows['N4NET'][-1]
                assert all(last.get(k,0)>0 for k in ('tcp','udp','http','mqtt')), last
                assert observed.rows['CONCUR'][-1]['frames'] > 1000
                if a.gestures:
                    assert gestures >= 9
            snapshot('end APP')
            status.update(status='passed', continuous_seconds=round(elapsed,3), gesture_cycles=gestures)
        except BaseException as e:
            status.update(status='failed', error=str(e))
            raise
        finally:
            status['rows'] = observed.rows
            status['scope'] = 'APP SDK workload; rendering durations are not physical panel FPS'
            (a.output/'result.json').write_text(json.dumps(status, indent=2)+'\n')
    print('N4_CAPTURE_PASS', json.dumps({k:v for k,v in status.items() if k!='rows'}), flush=True)


if __name__ == '__main__':
    main()
