#!/usr/bin/env python3
"""On-device downstream touch gate. Follow with physical two-finger testing.

Injects after the bus driver, never claims to test the I2C link. Owns the UART
exclusively; keep hands off during injection. Logs belong outside the repo.
Leaves the requested app running for physical end-to-end acceptance.
"""
import argparse
import re
import sys
import time
from pathlib import Path


class Console:
    def __init__(self, serial, log):
        self.serial, self.log = serial, log

    def command(self, line):
        self.serial.write(b'\x03\n')
        time.sleep(.04)
        self.serial.reset_input_buffer()
        self.serial.write(line.encode()+b'\n')
        self.serial.flush()
        data = bytearray()
        deadline, last = time.monotonic()+15, time.monotonic()
        while time.monotonic() < deadline:
            chunk = self.serial.read(8192)
            if chunk:
                data.extend(chunk)
                last = time.monotonic()
            text = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', data.decode(errors='replace'))
            compact = re.sub(r'\s+', '', text)
            echo = compact.find(''.join(line.split()))
            if (echo >= 0 and compact.rfind('aster:~$') > echo+len(''.join(line.split()))
                    and re.search(r'aster:~\$\s*$', text) and time.monotonic()-last > .15):
                self.log.write('\n> '+line+'\n'+text)
                self.log.flush()
                return text
        self.log.write('\nTIMEOUT '+line+'\n'+repr(bytes(data)))
        self.log.flush()
        raise RuntimeError('No completed response: '+line)

    def slot(self, slot, action, x, y):
        result = self.command(f'hw touch slot {slot} {action} {x} {y}')
        assert f'injected touch slot={slot} {action}' in result, result
        return result

    def state(self, expected):
        result = self.command('hw touch state')
        slots = {int(v) for v in re.findall(r'^touch slot=(\d+)', result, re.M)}
        assert slots == set(expected), (expected, result)
        assert f'touch active={len(expected)}' in result, result
        if 'touch recovery=' in result:
            assert 'touch recovery=0 ' in result, result

    def overlays(self, w, h, upward_close):
        x=w//2
        result=self.command(f'hw touch drag {x} {h-4} {x} {h-100} 8 30')
        assert 'return: confirmation opened' in result, result
        cw=min(w-32,288)
        cancel_x=(w-cw)//2+18+(cw-44)//4
        cancel_y=(h-194)//2+159
        result=self.command(f'hw touch tap {cancel_x} {cancel_y}')
        assert 'return: cancelled' in result, result
        result=self.command(f'hw touch drag {x} 4 {x} 180 8 30')
        assert 'qpanel: open (over app)' in result, result
        stroke=(f'{x} {h-4} {x} {h-150}' if upward_close else f'{x} 4 {x} 180')
        result=self.command(f'hw touch drag {stroke} 8 30')
        assert 'qpanel: close' in result, result
        self.state([])

    def replay(self, app):
        text=self.command('hw touch replay')
        assert 'driver replay result=0' in text,text
        deadline=time.monotonic()+4
        while 'AXS replay completed' not in text and time.monotonic()<deadline:
            chunk=self.serial.read(8192).decode(errors='replace')
            if chunk:
                self.log.write(chunk);self.log.flush();text+=chunk
        # Keep the post-replay APP pump and final UP delivery in the evidence.
        quiet_until=time.monotonic()+.2
        while time.monotonic()<quiet_until:
            chunk=self.serial.read(8192).decode(errors='replace')
            if chunk:
                self.log.write(chunk);self.log.flush();text+=chunk
        assert 'AXS replay completed' in text,text
        # Real contacts may resume as soon as the bounded override ends.
        # Do not count their events as part of the synthetic fault fixture.
        fixture=text.split('AXS replay completed',1)[0]
        for slot in (0,1):
            actions=[int(v) for v in re.findall(
                r'touch delivered app='+re.escape(app)+rf' slot={slot} action=(\d+)',fixture)]
            assert actions==[1,3,1,3],(slot,actions,fixture)
        held=fixture.split('AXS replay tick=10',1)[1].split('AXS replay tick=13',1)[0]
        assert not re.search(r'touch delivered .* action=1\b',held),held


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--sdk-tools', type=Path, required=True)
    p.add_argument('--log', type=Path, required=True)
    p.add_argument('--app', default='aster.gfxshow')
    p.add_argument('--rounds', type=int, default=20)
    p.add_argument('--overlays', action='store_true', help='check return cancel and panel close each round')
    p.add_argument('--trace', action='store_true', help='require diagnostic APP DOWN delivery after overlays')
    p.add_argument('--driver-replay', action='store_true', help='replay lost-UP fault at AXS driver report boundary; requires diagnostic firmware')
    a = p.parse_args()
    if not 1 <= a.rounds <= 100:
        p.error('--rounds must be 1..100')
    sys.path.insert(0, str(a.sdk_tools.resolve()))
    from aster_send import open_serial
    a.log.parent.mkdir(parents=True, exist_ok=True)
    with open_serial(a.port, timeout=.03, write_timeout=10, exclusive=True) as serial, a.log.open('w') as log:
        c = Console(serial, log)
        ready_deadline=time.monotonic()+30
        while True:
            info=c.command('aster sys info')
            if re.search(r'sd_mounted=yes apps=[1-9]\d*', info):
                break
            if time.monotonic()>=ready_deadline:
                raise RuntimeError('SD/app registry did not become ready')
            time.sleep(.5)
        c.command('aster sys mem')
        c.command('aster app start '+a.app)
        assert 'RUNNING' in c.command('aster app state '+a.app)
        info = c.command('hw display info')
        match = re.search(r'display0: (\d+)x(\d+)', info)
        assert match, info
        w, h = map(int, match.groups())
        # Keep the first finger at the right edge: its pending return gesture
        # must be canceled when the second finger joins, in both release orders.
        x0, x1, y = w-5, w//3, h//2
        try:
            c.state([])
            for i in range(a.rounds):
                if a.driver_replay:
                    c.replay(a.app)
                    c.state([])
                    assert 'RUNNING' in c.command('aster app state '+a.app)
                    print(f'driver replay {i+1}/{a.rounds}: stale CONTACT rejected, fresh DOWN recovered, both slots released',flush=True)
                    continue
                first, second = ((0,1) if i%2 == 0 else (1,0))
                release, remaining = ((first,second) if i%4<2 else (second,first))
                c.slot(first,'down',x0,y)
                c.state([first])
                c.slot(second,'down',x1,y)
                time.sleep(.12)
                c.state([0,1])
                c.slot(first,'move',w//2,y)
                c.slot(second,'move',w//3,y+20)
                c.slot(release,'up',w//2,y)
                c.state([remaining])
                c.slot(remaining,'move',w//2,y+10)
                c.slot(remaining,'up',w//2,y+10)
                c.state([])
                assert 'RUNNING' in c.command('aster app state '+a.app)
                if a.overlays:
                    c.overlays(w,h,bool(i%2))
                    result=c.slot(0,'down',w//2,h//2)
                    if a.trace:
                        assert re.search(r'touch delivered app='+re.escape(a.app)+r' slot=0 action=1\b',result),result
                    c.slot(0,'up',w//2,h//2)
                    c.state([])
                    assert 'RUNNING' in c.command('aster app state '+a.app)
                print(f'round {i+1}/{a.rounds}: slots, independent releases, no edge exit OK', flush=True)
        finally:
            # Explicitly release both slots even if an assertion fails.
            for slot in (0,1):
                c.slot(slot,'up',w//2,y)
        c.command('aster sys mem')
        print(('DRIVER REPLAY' if a.driver_replay else 'INJECTION')+
              ' PASS; physical contact acceptance still required', flush=True)


if __name__ == '__main__':
    main()
