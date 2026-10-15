#!/usr/bin/env python3
"""Capture native Zephyr pool evidence with one exclusively owned SDK serial port.

Requires config/diagnostics/network-pools.conf. No credentials are read or sent.
Connection maxima are sampled lower bounds; net mem MaxUsed is allocator-owned
boot high water. Do not interpret the two as the same measurement.
"""
import argparse
import json
import re
import sys
import time
from pathlib import Path


def counts(text):
    contexts, rest = text.split('Handler', 1)
    handlers = rest.split('\nTCP ', 1)[0]
    row = r'^\[\s*\d+\] '
    return len(re.findall(row, contexts, re.M)), len(re.findall(row, handlers, re.M))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk-tools', type=Path, required=True)
    p.add_argument('--port', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--seconds', type=int, default=30)
    p.add_argument('--dhcp-restart', action='store_true',
                   help='Restart DHCP on interface 1 once; may interrupt APP sockets')
    args = p.parse_args()
    if not 5 <= args.seconds <= 120:
        p.error('--seconds must be 5..120')
    sys.path.insert(0, str(args.sdk_tools.resolve()))
    from aster_send import open_serial
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    monitor = {}
    with open_serial(args.port, timeout=.03, write_timeout=5, exclusive=True) as serial, \
            (args.output/'serial.txt').open('w', encoding='utf-8') as log:
        def command(s):
            serial.write(b'\x03\n'); time.sleep(.04); serial.reset_input_buffer()
            serial.write((s+'\n').encode()); serial.flush()
            out = bytearray(); deadline = time.monotonic()+15; last_data = time.monotonic()
            while time.monotonic() < deadline:
                chunk = serial.read(8192)
                if chunk:
                    out.extend(chunk); last_data = time.monotonic()
                text = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', out.decode(errors='replace'))
                if s in text and text.rstrip().endswith('aster:~$') and time.monotonic()-last_data >= .12:
                    log.write(f'\n> {s}\n{text}\n'); log.flush()
                    return text
            raise RuntimeError('serial command timeout: '+s)
        for s in ['aster sys info', 'hw wifi status', 'aster net stats',
                  'netpool', 'net conn', 'net mem', 'net stats', 'aster sys mem']:
            command(s)
        start = time.monotonic(); queried = set(); restarted = False
        while time.monotonic()-start < args.seconds:
            elapsed = time.monotonic()-start
            if args.dhcp_restart and not restarted and elapsed >= 2:
                command('net dhcpv4 client start 1'); restarted = True
            query = int(elapsed)//4
            if query not in queried:
                # Distinct names avoid a positive DNS cache hit. NXDOMAIN is a
                # valid resolved outcome, not evidence of pool exhaustion.
                command(f'net dns query n1-{time.time_ns()}.example.com')
                queried.add(query)
            snapshot = command('net conn')
            context_count, handler_count = counts(snapshot)
            records.append(dict(seconds=round(time.monotonic()-start,3),
                                contexts=context_count, handlers=handler_count))
            time.sleep(.08)
        for s in ['netpool', 'net conn', 'net mem', 'net stats', 'hw wifi status',
                  'aster net stats', 'aster sys mem', 'kernel thread stacks']:
            text = command(s)
            if s == 'netpool':
                monitor = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', text)}
    result = dict(monitor=monitor, samples=len(records),
                  contexts_observed_max=max(r['contexts'] for r in records),
                  handlers_observed_max=max(r['handlers'] for r in records),
                  rows=records)
    (args.output/'summary.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'}))


if __name__ == '__main__':
    main()
