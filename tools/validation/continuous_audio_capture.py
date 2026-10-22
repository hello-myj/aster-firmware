#!/usr/bin/env python3
"""Observe real APP/renderer/effect/I2S service on one board; does not prove sound.

Keep hands off during the baseline. Audible continuity requires separate human
acceptance. DMA live-depth/hardware-underrun telemetry is explicitly unknown.
Logs go outside the repository. Leaves the example running for listening.
"""
import argparse
import re
import sys
import time
from pathlib import Path
from multitouch_capture import Console


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--sdk-tools', type=Path, required=True)
    p.add_argument('--log', type=Path, required=True)
    p.add_argument('--seconds', type=float, default=600)
    args = p.parse_args()
    if args.seconds < 1:
        p.error('seconds must be positive')
    sys.path.insert(0, str(args.sdk_tools.resolve()))
    from aster_send import open_serial
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with open_serial(args.port, timeout=.03, write_timeout=10, exclusive=True) as serial, args.log.open('w') as log:
        c = Console(serial, log)
        assert 'abi=4.1.0' in c.command('aster sys info')
        assert 'RUNNING' in c.command('aster app state aster.example.continuousaudio')
        c.command('aster sys mem')
        start = time.monotonic()
        next_snapshot = next_progress = start
        phases = set()
        snapshots = 0
        while time.monotonic() - start < args.seconds:
            raw = serial.read(8192).decode(errors='replace')
            if raw:
                log.write(raw)
                log.flush()
                assert not re.search(r'player (feed|refill)|FATAL|ASSERT|CPU.*Exception', raw), raw
            now = time.monotonic()
            if now >= next_snapshot:
                result = c.command('hw audio fxstate')
                assert 'loaded=0x1 playing=0x1 suspended=0 pcm_bytes=4096' in result, result
                peak = re.search(r'mixed_peak=(\d+)', result)
                assert peak and int(peak[1]) > 0, result
                frame = re.search(r'slot=1 rendered_source_frame=(\d+) fraction_q16=(\d+)', result)
                assert frame, result
                phases.add(tuple(map(int, frame.groups())))
                snapshots += 1
                next_snapshot = now + 13.17
            if now >= next_progress:
                print(f'loop baseline {now-start:.1f}/{args.seconds:.1f}s; nonzero PCM snapshots={snapshots}', flush=True)
                next_progress = now + 30
        assert len(phases) > 1, 'No observed source phase advancement'
        assert 'RUNNING' in c.command('aster app state aster.example.continuousaudio')
        c.command('aster sys mem')
        print(f'PASS software observations: {time.monotonic()-start:.2f}s, {snapshots} snapshots; listening and hardware underruns not asserted', flush=True)


if __name__ == '__main__':
    main()
