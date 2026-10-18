#!/usr/bin/env python3
"""N5.1 G1 supplement: cases the base capture/concurrent gates do not cover.

Covers DNS failure, unreachable port (refused + connect timeout), cancel in
connect/ready stages, stale-handle isolation, slow bytewise frame, fragment
assembly deadline, slow ordered consumer incl. empty message, interleaved
control frames, single-client exhaustion, blackholed peer liveness deadline
and repeated open/close lifecycle with resource return to baseline.
Firmware errno values are private; assert stable state/error signs, not Linux
numbers. Exclusive UART; logs/results stay outside Git.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import time
import zlib
from network_http_capture import Client


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--host', required=True)
    p.add_argument('--sdk-tools', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cycles', type=int, default=25)
    p.add_argument('--skip-slow', action='store_true',
                   help='skip the ~35 s fragment-deadline and ~30 s blackhole cases')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(a.sdk_tools.resolve()))
    from aster_send import open_serial
    results = {'status': 'running', 'cases': [],
               'scope': 'Firmware CLI G1 supplement; not SDK/APP or physical display acceptance'}
    handles = set()
    with open_serial(a.port, timeout=.03, write_timeout=10, exclusive=True) as ser, \
            (a.output / 'serial.txt').open('w') as log:
        c = Client(ser, log)

        def command(text):
            end = time.monotonic() + 12
            while True:
                value = c.command(text)
                print(value, flush=True)
                # Bounded EAGAIN retry on lock contention; never resend a
                # side effect that was already accepted.
                if not re.search(r'^WS result=-11\r?$', value, re.M) or time.monotonic() >= end:
                    return value
                time.sleep(.1)

        def ok(text):
            value = command(text)
            assert re.search(r'^WS result=0\r?$', value, re.M), value

        def baseline():
            for _ in range(80):
                if 'active=0 closing=0 tls=0 psram=0' in command('aster net ws stats'):
                    return
                time.sleep(.1)
            raise AssertionError('WS did not return to baseline')

        def snap(h):
            v = command(f'aster net ws status {h}')
            m = re.search(r'^WS handle=\d+ state=.*$', v, re.M)
            assert m, v
            return {k: int(n) for k, n in re.findall(r'(\w+)=(-?\d+)', m.group(0))}

        def wait(h, test, seconds=20):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                s = snap(h)
                if test(s):
                    return s
                time.sleep(.15)
            raise AssertionError('WS state deadline: ' + str(s))

        def start(path='/echo', tls=False, port=None, timeout=15000, host=None):
            target = host or a.host
            value = command(
                f"aster net ws open {'wss' if tls else 'ws'}://{target}:{port or (17881 if tls else 17880)}{path} {timeout}")
            m = re.search(r'^WS handle=(\d+)', value, re.M)
            assert m, value
            h = int(m[1])
            handles.add(h)
            return h

        def close(h):
            ok(f'aster net ws close {h}')
            handles.discard(h)
            baseline()

        def receive(h, capacity=4096, seconds=15):
            end = time.monotonic() + seconds
            while True:
                value = command(f'aster net ws recv {h} {capacity}')
                if not re.search(r'^WS handle=\d+ result=-11 ', value, re.M) or time.monotonic() >= end:
                    break
                time.sleep(.1)
            m = re.search(r'^WS handle=\d+ result=(-?\d+) opcode=(\d+) required=(\d+)', value, re.M)
            assert m, value
            rc, op, n = map(int, m.groups())
            data = b''.join(bytes.fromhex(x) for x in
                            re.findall(r'^WS data offset=\d+ hex=([0-9a-f]+)', value, re.M))
            if rc == 0:
                assert len(data) == n and f'crc32={zlib.crc32(data):08x}' in value, value
            return rc, op, n, data

        def failed_open(name, expect_error=True, **kw):
            h = start(**kw)
            s = wait(h, lambda s: s['state'] == 7, seconds=kw.get('timeout', 15000) / 1000 + 10)
            if expect_error:
                assert s['error'] < 0, s
            close(h)
            results['cases'].append({'name': name, 'passed': True,
                                     'error': s['error'], 'transport_error': s.get('transport_error')})
            print('WS_CASE_PASS', name, flush=True)
            return s

        try:
            for text in ['aster sys info', 'hw wifi status', 'aster net ws stats', 'aster sys mem']:
                command(text)
            baseline()
            mem_before = command('aster sys mem')

            # 1. DNS failure: NXDOMAIN must surface as a failed client, not a hang.
            failed_open('dns_failure', host='aster-n5-missing.invalid', timeout=8000)

            # 2. Unreachable port: refused on LAN, connect deadline on TEST-NET.
            failed_open('unreachable_port_refused', port=17899, timeout=5000)
            s = failed_open('connect_timeout_testnet', host='192.0.2.1', timeout=2000)
            assert s['error'] == -116, s

            # 3. Cancel while still connecting (no route): CANCELLED terminal.
            h = start(host='192.0.2.1', timeout=15000)
            time.sleep(1.5)
            ok(f'aster net ws cancel {h}')
            s = wait(h, lambda s: s['state'] == 8)
            assert s['error'] < 0, s
            close(h)
            results['cases'].append({'name': 'cancel_in_connect', 'passed': True, 'error': s['error']})
            print('WS_CASE_PASS cancel_in_connect', flush=True)

            # 4. Cancel in READY with the peer healthy.
            h = start()
            wait(h, lambda s: s['state'] == 4)
            ok(f'aster net ws cancel {h}')
            s = wait(h, lambda s: s['state'] == 8)
            close(h)
            results['cases'].append({'name': 'cancel_in_ready', 'passed': True, 'error': s['error']})
            print('WS_CASE_PASS cancel_in_ready', flush=True)

            # 5. Stale handle isolation after close+reopen (generation guard).
            h1 = start()
            wait(h1, lambda s: s['state'] == 4)
            close(h1)
            h2 = start()
            wait(h2, lambda s: s['state'] == 4)
            stale = command(f'aster net ws status {h1}')
            assert re.search(r'^WS result=-9\r?$', stale, re.M), stale
            stale = command(f'aster net ws send {h1} 2 -')
            assert re.search(r'^WS result=-9\r?$', stale, re.M), stale
            close(h2)
            results['cases'].append({'name': 'stale_handle_isolation', 'passed': True,
                                     'h1': h1, 'h2': h2})
            print('WS_CASE_PASS stale_handle_isolation', flush=True)

            # 6. Slow bytewise frame completes inside the bounded assembly window.
            h = start('/slow')
            wait(h, lambda s: s['state'] == 4)
            rc, op, n, data = receive(h, seconds=15)
            assert rc == 0 and op == 2 and data == bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66]), (rc, op, data)
            close(h)
            results['cases'].append({'name': 'slow_frame_bytewise', 'passed': True, 'bytes': n})
            print('WS_CASE_PASS slow_frame_bytewise', flush=True)

            # 7. Slow consumer: ordered mixed batch incl. an empty message.
            h = start('/multi')
            wait(h, lambda s: s['state'] == 4)
            time.sleep(2)
            # First message is 1 byte: recv fits exactly.
            rc, op, n, data = receive(h)
            assert rc == 0 and op == 2 and data == bytes([0xa1]), (rc, op, n, data)
            # Second message is 2 bytes: a capacity=1 recv must report the
            # required length/opcode WITHOUT consuming, so a full recv then
            # returns the same message intact (message boundary preserved).
            rc, op, n, data = receive(h, capacity=1)
            assert rc == -122 and op == 2 and n == 2, (rc, op, n)
            rc, op, n, data = receive(h)
            assert rc == 0 and op == 2 and data == bytes([0xb1, 0xb2]), (rc, op, n, data)
            # Remaining: empty binary, 3-byte binary, text tail — in order.
            expected = [(2, bytes([0xa1])), (2, bytes([0xb1, 0xb2])), (2, b''),
                        (2, bytes([0xc1, 0xc2, 0xc3])), (1, '尾帧'.encode())]
            for want_op, want in expected[2:]:
                rc, op, n, data = receive(h)
                assert rc == 0 and op == want_op and data == want, (rc, op, n, data, want)
            rc, op, n, data = receive(h, seconds=2)
            assert rc == -11, (rc, op, n)  # drained: no message, distinct from empty
            close(h)
            results['cases'].append({'name': 'slow_consumer_ordered', 'passed': True,
                                     'messages': len(expected)})
            print('WS_CASE_PASS slow_consumer_ordered', flush=True)

            # 8. Interleaved control frames: auto-pong keeps data flowing.
            #    Head-of-line is bounded by design: an unread data frame blocks
            #    later data frames (single message slot), so the consumer must
            #    drain d1..d3 before the pong-marker frame becomes reachable.
            h = start('/ping')
            wait(h, lambda s: s['state'] == 4)
            for want in (bytes([0xd1]), bytes([0xd2]), bytes([0xd3])):
                wait(h, lambda s: s['queued'] == 1)
                rc, op, n, data = receive(h)
                assert rc == 0 and op == 2 and data == want, (rc, op, data, want)
            s = wait(h, lambda s: s['pings'] >= 3 and s['queued'] == 1, seconds=25)
            rc, op, n, data = receive(h)
            assert rc == 0 and op == 2 and data == bytes([3]), (rc, op, data)
            close(h)
            results['cases'].append({'name': 'control_frames_auto_pong', 'passed': True,
                                     'pings': s['pings']})
            print('WS_CASE_PASS control_frames_auto_pong', flush=True)

            # 9. Single-client exhaustion: a second open is refused, then recovers.
            h1 = start()
            wait(h1, lambda s: s['state'] == 4)
            second = command(f'aster net ws open ws://{a.host}:17880/echo 5000')
            assert re.search(r'^WS result=-28\r?$', second, re.M), second
            second = command(f'aster net ws open wss://{a.host}:17881/echo 5000')
            assert re.search(r'^WS result=-28\r?$', second, re.M), second
            close(h1)
            h2 = start()
            wait(h2, lambda s: s['state'] == 4)
            close(h2)
            results['cases'].append({'name': 'single_client_exhaustion', 'passed': True})
            print('WS_CASE_PASS single_client_exhaustion', flush=True)

            if not a.skip_slow:
                # 10. Fragment assembly deadline: first fragment then silence.
                h = start('/slowfrag')
                wait(h, lambda s: s['state'] == 4)
                s = wait(h, lambda s: s['state'] == 7, seconds=45)
                assert s['error'] == -116, s
                close(h)
                results['cases'].append({'name': 'fragment_assembly_deadline', 'passed': True,
                                         'error': s['error']})
                print('WS_CASE_PASS fragment_assembly_deadline', flush=True)

                # 11. Blackholed peer: idle ping gets no pong; liveness deadline
                #     fires while CLI stays responsive (commands interleave).
                h = start('/blackhole')
                wait(h, lambda s: s['state'] == 4)
                command('aster sys mem')
                s = wait(h, lambda s: s['state'] == 7, seconds=45)
                assert s['error'] == -116, s
                close(h)
                results['cases'].append({'name': 'blackhole_liveness_deadline', 'passed': True,
                                         'error': s['error']})
                print('WS_CASE_PASS blackhole_liveness_deadline', flush=True)

            # 12. Repeated open/close lifecycle; resources must return each round.
            started = time.monotonic()
            for i in range(a.cycles):
                h = start()
                wait(h, lambda s: s['state'] == 4)
                ok(f'aster net ws pattern {h} 128 {i % 251}')
                wait(h, lambda s: s['queued'] == 1)
                rc, op, n, data = receive(h)
                assert rc == 0 and data == bytes((j + i % 251) % 251 for j in range(128)), (rc, n)
                close(h)
            elapsed = time.monotonic() - started
            command('aster sys mem')
            command('aster net stats')
            command('netpool')
            results['cases'].append({'name': 'lifecycle_cycles', 'passed': True,
                                     'cycles': a.cycles, 'seconds': round(elapsed, 1)})
            print('WS_CASE_PASS lifecycle_cycles', flush=True)

            command('aster sys mem')
            command('aster net ws stats')
            command('aster net http stats')
            command('aster net stats')
            command('netpool')
            command('kernel thread stacks')
            results['status'] = 'passed'
        except BaseException as error:
            results.update(status='failed', error=str(error))
            raise
        finally:
            for h in list(handles):
                try:
                    command(f'aster net ws cancel {h}')
                    close(h)
                except Exception as error:
                    log.write('\nCLEANUP_FAILED ' + repr(error) + '\n')
            (a.output / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
    assert results['status'] == 'passed', results
    print('WS_G1_EXTRA_PASS', flush=True)


if __name__ == '__main__':
    main()
