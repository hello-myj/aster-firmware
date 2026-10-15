#!/usr/bin/env python3
"""N2 CLI gate against network_http_server.cjs; exclusively own the shared UART.

No passwords or private keys. CA file must already be on the SD card. Write raw
logs/results outside the repository. Firmware errors are private errno values;
assert status/content/certificate flags instead of assuming Linux errno numbers.
"""
import argparse
import hashlib
import json
import re
import sys
import time
import zlib
from pathlib import Path


class Client:
    def __init__(self, serial, log):
        self.serial, self.log = serial, log

    def command(self, line):
        self.serial.write(b'\x03\n')
        time.sleep(.04)
        self.serial.reset_input_buffer()
        wire = line.encode()
        for offset in range(0, len(wire), 24):
            self.serial.write(wire[offset:offset+24])
            self.serial.flush()
            time.sleep(.01)
        # Keep Enter separate from a full USB packet during animated APPs.
        time.sleep(.02)
        self.serial.write(b'\n')
        self.serial.flush()
        out = bytearray()
        deadline, last_data = time.monotonic()+15, time.monotonic()
        while time.monotonic() < deadline:
            chunk = self.serial.read(8192)
            if chunk:
                out.extend(chunk)
                last_data = time.monotonic()
            text = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', out.decode(errors='replace'))
            prefix = 'HTTP' if line.startswith('aster net http ') else 'NET' if line.startswith('aster net ') else None
            response = re.search(r'^'+prefix+r' ', text, re.M) if prefix else None
            # Terminal wraps can split command echo across physical lines.
            echoed = ''.join(line.split()) in re.sub(r'\s+', '', text)
            prompt_after = text.rfind('aster:~$') > (response.start() if response else -1)
            if echoed and prompt_after and (not prefix or response) and time.monotonic()-last_data >= .12:
                self.log.write('\n> '+line+'\n'+text+'\n')
                self.log.flush()
                return text
        self.log.write('\nTIMEOUT > '+line+'\n'+repr(bytes(out))+'\n')
        self.log.flush()
        raise RuntimeError('serial timeout: '+line)

    def command_ok(self, line):
        result = self.command(line)
        assert re.search(r'^HTTP result=0\r?$', result, re.M), result
        return result

    def status(self, h):
        text = self.command(f'aster net http result {h}')
        record = re.search(r'^HTTP handle=\d+ state=.*$', text, re.M)
        assert record, text
        return {k:int(v, 16 if k == 'verify' else 10) for k,v in re.findall(r'(\w+)=(-?[0-9a-f]+)', record[0])}

    def read(self, h):
        text = self.command(f'aster net http read {h} 1200')
        rc = int(re.search(r'^HTTP handle=\d+ result=(-?\d+)', text, re.M)[1])
        data = b''.join(bytes.fromhex(b) for b in re.findall(r'^HTTP data offset=\d+ hex=([0-9a-f]*)', text, re.M))
        if rc >= 0:
            assert len(data) == rc and f'crc32={zlib.crc32(data):08x}' in text, text
        return rc, data

    def start(self, url, method='GET', body=None, limit=1048576, timeout=15000, headers=None):
        command = f'aster net http start {method} {url} {timeout} {limit}'
        if body is not None or headers:
            assert not body or re.fullmatch(r'[A-Za-z0-9._-]+', body)
            command += ' '+(body or '-')
        if headers:
            command += ' '+headers
        for _ in range(30):
            text = self.command(command)
            match = re.search(r'^HTTP handle=(\d+) queued=1', text, re.M)
            if match:
                return int(match[1])
            # Resource teardown may still own the two transport slots.
            if not re.search(r'^HTTP result=-(11|28)\r?$', text, re.M):
                raise AssertionError(text)
            time.sleep(.1)
        raise AssertionError('admission did not recover')

    def close(self, h):
        self.command_ok(f'aster net http close {h}')
        # Native TCP has a bounded TIME_WAIT window; do not call it a leak.
        time.sleep(1.6)
        assert 'active=0 closing=0 tls=0 psram=0' in self.command('aster net http stats')

    def fetch(self, url, method='GET', body=None, limit=1048576, timeout=15000, headers=None, slow=False):
        h = self.start(url, method, body, limit, timeout, headers)
        data = bytearray()
        started = time.monotonic()
        try:
            if slow:
                time.sleep(2)
                snap = self.status(h)
                assert snap['queued'] == 2048 and snap['state'] == 5, snap
                self.command('aster net http stats')
                self.command('aster sys mem')
            while time.monotonic()-started < timeout/1000+15:
                rc, chunk = self.read(h)
                if rc > 0:
                    data.extend(chunk)
                    continue
                snap = self.status(h)
                if snap['state'] >= 6 and snap['queued'] == 0:
                    expected_rc = 0 if snap['state'] == 6 else snap['error']
                    if rc != expected_rc:
                        continue  # Worker may have completed after the preceding read.
                    snap['read_result'] = rc
                    snap['seconds'] = round(time.monotonic()-started, 3)
                    snap['sha256'] = hashlib.sha256(data).hexdigest()
                    return bytes(data), snap
                time.sleep(.05)
            raise AssertionError('request did not terminate')
        finally:
            self.close(h)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk-tools', type=Path, required=True)
    p.add_argument('--port', required=True)
    p.add_argument('--host', required=True)
    p.add_argument('--ca-file', default='/SD:/aster/n2-ca.pem')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--pools', action='store_true', help='Capture native allocator high water and sampled contexts')
    args = p.parse_args()
    sys.path.insert(0, str(args.sdk_tools.resolve()))
    from aster_send import open_serial
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    with open_serial(args.port, timeout=.03, write_timeout=5, exclusive=True) as serial, (args.output/'serial.txt').open('w') as log:
        c = Client(serial, log)
        for line in ['aster sys info', 'hw wifi status', 'aster net stats', 'aster net http stats', 'aster sys mem']:
            c.command(line)
        if args.pools:
            c.command('netpool reset')
            c.command('net mem')
        base = 'http://'+args.host+':9080'
        secure = 'https://'+args.host
        pattern = lambda n: bytes(i % 251 for i in range(n))
        def check(name, url, expected=None, status=200, failure=False, **options):
            data, result = c.fetch(url, **options)
            assert result['state'] == (7 if failure else 6), result
            if failure:
                assert result['error'] < 0 and result['read_result'] < 0, result
            else:
                assert result['status'] == status and result['error'] == 0 and result['read_result'] == 0, result
            if expected is not None:
                assert data == expected, (name, len(data), result)
            records.append(dict(name=name, **result))
            (args.output/'summary.json').write_text(json.dumps(records, indent=2)+'\n')
            print(f'PASS {name}: status={result["status"]} bytes={len(data)} error={result["error"]} verify={result["verify"]:08x}', flush=True)
        check('fixed', base+'/fixed?n=4096', pattern(4096))
        # The deadline includes intentional backpressure and paced serial hex output.
        # Keep the separate short-deadline case below to verify timeout behavior.
        check('large-slow-reader', base+'/fixed?n=65536', pattern(65536), slow=True, timeout=60000)
        check('chunked', base+'/chunked?n=8192', pattern(8192))
        check('eof', base+'/eof?n=4096', pattern(4096))
        check('head', base+'/fixed?n=4096', b'', method='HEAD')
        check('post', base+'/echo', b'hello-n2', method='POST', body='hello-n2')
        check('404', base+'/status/404?n=8', pattern(8), status=404)
        check('500', base+'/status/500?n=8', pattern(8), status=500)
        check('redirect', base+'/redirect?to=/fixed?n=8', pattern(8))
        check('truncated', base+'/truncated?n=8', pattern(8), failure=True)
        check('body-limit', base+'/fixed?n=8', b'', failure=True, limit=7)
        check('total-timeout', base+'/stall', b'', failure=True, timeout=1000)
        c.command_ok('aster net http ca '+args.ca_file)
        check('https-valid', secure+':9443/fixed?n=4096', pattern(4096))
        check('https-chunked', secure+':9443/chunked?n=8192', pattern(8192))
        check('https-name-mismatch', secure+':9444/fixed?n=8', b'', failure=True)
        assert records[-1]['verify'] not in (0, 0xffffffff), records[-1]
        check('https-expired', secure+':9445/fixed?n=8', b'', failure=True)
        assert records[-1]['verify'] not in (0, 0xffffffff), records[-1]
        c.command_ok('aster net http ca default')
        check('https-untrusted', secure+':9443/fixed?n=8', b'', failure=True)
        assert records[-1]['verify'] not in (0, 0xffffffff), records[-1]
        c.command_ok('aster net http ca '+args.ca_file)
        check('https-recovery', secure+':9443/fixed?n=8', pattern(8))
        for url in [base+'/stall', secure+':9446/stall', base+'/fixed?n=65536']:
            h = c.start(url, timeout=30000)
            time.sleep(.5)
            c.command_ok(f'aster net http cancel {h}')
            for _ in range(30):
                snap = c.status(h)
                if snap['state'] >= 6:
                    break
                time.sleep(.1)
            assert snap['state'] == 8 and snap['error'] < 0, snap
            records.append(dict(name='cancel', url=url, **snap))
            c.close(h)
        c.command_ok('aster net http ca default')
        if args.pools:
            for line in ['netpool', 'net conn', 'net mem', 'net stats']:
                c.command(line)
        for line in ['aster net http stats', 'aster net stats', 'aster sys mem', 'kernel thread stacks']:
            c.command(line)
        (args.output/'summary.json').write_text(json.dumps(records, indent=2)+'\n')
        print(f'N2 CLI functional cases PASS: {len(records)} (not full N2 acceptance)', flush=True)


if __name__ == '__main__':
    main()
