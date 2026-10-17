#!/usr/bin/env python3
"""N4 post-run APP-exit cycles, Wi-Fi return and settled resource evidence.

Run after the continuous gate and physical confirmation. Uses the temporary
N4 APP only; leaves it installed but stopped. Caller restores normal firmware,
original foreground/volume/network and removes temporary files/certificates.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import time
from network_http_capture import Client
from network_n4_capture import APP, ObservedSerial


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--sdk-tools', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--width', type=int, required=True)
    p.add_argument('--height', type=int, required=True)
    p.add_argument('--cycles', type=int, default=3)
    a = p.parse_args()
    if a.cycles < 3:
        p.error('at least three exit cycles are required')
    sys.path.insert(0, str(a.sdk_tools.resolve()))
    from aster_send import open_serial
    a.output.mkdir(parents=True, exist_ok=True)
    results = {'status':'running','cycles':[]}
    with open_serial(a.port, timeout=.1, write_timeout=10, exclusive=True) as s, \
         (a.output/'serial.txt').open('w') as raw, (a.output/'commands.txt').open('w') as log:
        observed = ObservedSerial(s, raw)
        c = Client(observed, log)
        def command(line):
            text = c.command(line)
            if observed.failure:
                raise AssertionError(observed.failure)
            print(text, flush=True)
            return text
        def watch(seconds):
            until = time.monotonic()+seconds
            while time.monotonic() < until:
                observed.read(8192)
                if observed.failure:
                    raise AssertionError(observed.failure)
        def settled(label):
            for _ in range(30):
                service = [command('aster net '+kind+'stats') for kind in ('mqtt ','http ','')]
                if all('active=0 closing=0' in v and 'psram=0' in v for v in service):
                    break
                watch(1)
            else:
                raise AssertionError('service resources did not settle')
            watch(2)  # native TIME_WAIT is distinct from logical owner release
            memory = command('aster sys mem')
            pool = command('netpool')
            command('net mem')
            command('net conn')
            record = {'label':label,'services':service,'memory':memory,'pool':pool}
            for name in ('dram','libc'):
                match = re.search(name+r' heap:.*?used=(\d+)', memory)
                assert match, memory
                record[name+'_used'] = int(match[1])
            match = re.search(r'largest free block >= (\d+)', memory)
            record['psram_largest_lower_bound'] = int(match[1]) if match else None
            return record
        try:
            # Start from a known overlay-free state, not a previous unclosed sheet.
            command('aster app stop '+APP)
            baseline = settled('before cycles')
            results['baseline'] = baseline
            for n in range(a.cycles):
                observed.failure = None
                observed.armed = True
                command('aster app start '+APP)
                assert 'RUNNING' in command('aster app state '+APP)
                watch((3, 23, 43)[n % 3])
                if n == 0:
                    x = a.width//2
                    command(f'hw touch drag {x} 4 {x} 180 8 30')
                    watch(1)
                    command(f'hw touch tap {a.width-52} 59')
                    watch(3)  # Wi-Fi editor performs a real scan; no credential edits
                    (a.output/'wifi-screenshot.txt').write_text(command('aster ui screenshot'))
                    command(f'hw touch tap {a.width-31} 20')
                    watch(1)
                    assert 'RUNNING' in command('aster app state '+APP)
                    results['wifi_return_injected'] = True
                active = {kind:command('aster net '+kind+'stats') for kind in ('mqtt ','http ','')}
                active['memory'] = command('aster sys mem')
                start = time.monotonic()
                command('aster app stop '+APP)
                assert 'STOPPED' in command('aster app state '+APP)
                observed.armed = False
                record = settled('after exit '+str(n+1))
                record['exit_and_settle_seconds'] = round(time.monotonic()-start, 3)
                record['active_before_exit'] = active
                results['cycles'].append(record)
                print('EXIT_CYCLE_PASS',n+1,flush=True)
            # Worker stacks were warmed by the preceding full run. UI caches and
            # native NTP activity are reported separately, not waved away as leaks.
            libc = [v['libc_used'] for v in results['cycles']]
            assert len(set(libc)) == 1 and libc[-1] == baseline['libc_used'], libc
            results['status'] = 'passed'
            results['scope'] = ('Service ownership/handles, stable libc usage, kernel usage and coarse PSRAM allocation probes. '
                                'Wi-Fi actions are injected; confirm the actual page and full restoration visually. '
                                'PSRAM probes cannot rule out small allocator leaks.')
        except BaseException as error:
            results.update(status='failed',error=str(error))
            raise
        finally:
            (a.output/'result.json').write_text(json.dumps(results,indent=2)+'\n')
    print('N4_LIFECYCLE_PASS',flush=True)


if __name__ == '__main__':
    main()
