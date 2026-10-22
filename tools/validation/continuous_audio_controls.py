#!/usr/bin/env python3
"""Physical-board follow-up to continuous_audio_capture.py.

Run after the untouched baseline. lifecycle leaves the SDK example running;
reclaim and eof require a stopped APP and preuploaded, self-generated samples.
Uses the real filesystem, player worker, mixer and I2S service, not a DSP stub.
"""
import argparse
import re
import sys
import time
from pathlib import Path
from multitouch_capture import Console

APP = 'aster.example.continuousaudio'

def phase(text):
    m = re.search(r'slot=1 rendered_source_frame=(\d+) fraction_q16=(\d+)', text)
    assert m, text
    return tuple(map(int, m.groups()))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--sdk-tools', type=Path, required=True)
    p.add_argument('--log', type=Path, required=True)
    p.add_argument('--stage', choices=('lifecycle','reclaim','eof'), required=True)
    p.add_argument('--sample', help='absolute device WAV path for reclaim or looping effect')
    p.add_argument('--music', help='absolute device BGM path; generated sample must last 6 seconds')
    p.add_argument('--rate', type=int, default=16000)
    args = p.parse_args()
    if args.stage in ('reclaim','eof') and not args.sample:
        p.error('--sample is required')
    if args.stage == 'eof' and not args.music:
        p.error('--music is required')
    sys.path.insert(0, str(args.sdk_tools.resolve()))
    from aster_send import open_serial
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with open_serial(args.port, timeout=.03, write_timeout=10, exclusive=True) as serial, args.log.open('w') as log:
        c=Console(serial,log)
        c.command('aster sys info')
        def fx(command):
            result=c.command('hw audio '+command)
            assert 'result=0' in result,result
            return result
        def state():
            return c.command('hw audio fxstate')
        if args.stage=='lifecycle':
            assert 'RUNNING' in c.command('aster app state '+APP)
            fx('fxparams 1 98304 32768 120')
            c.command('aster app pause '+APP);time.sleep(.5)
            a=state();assert 'playing=0x1 suspended=1' in a,a
            time.sleep(1);b=state();assert phase(a)==phase(b),(a,b)
            c.command('aster app resume '+APP);time.sleep(.5)
            a=state();assert 'playing=0x1 suspended=0' in a,a
            time.sleep(.317);b=state();assert phase(a)!=phase(b),(a,b)
            fx('fxstop 1 80');time.sleep(.2)
            a=state();assert 'loaded=0x1 playing=0x0' in a,a
            fx('fxstop 1 0');fx('fxloop 1');fx('fxparams 1 65536 32768 30')
            c.overlays(320,480,True)
            assert 'playing=0x1 suspended=0' in state()
            print('PASS: native params, frozen pause phase, resume, independent stop, overlays; example remains running',flush=True)
        elif args.stage=='reclaim':
            c.command('hw audio stop');time.sleep(2.2)
            c.command('aster sys mem')
            for i in range(50):
                assert 'fxload handle=1' in c.command('hw audio fxload '+args.sample)
                fx('fxloop 1');fx('fxfree 1')
                assert 'loaded=0x0 playing=0x0' in state()
                if (i+1)%10==0: print(f'physical allocation/play/unload cycles {i+1}/50',flush=True)
            time.sleep(2.2)
            assert 'state=idle' in c.command('hw audio state')
            c.command('aster sys mem')
            print('PASS: 50 physical slot cycles, no live PCM and worker returned idle',flush=True)
        else:
            assert 'fxload handle=1' in c.command('hw audio fxload '+args.sample)
            try:
                c.command('hw audio start '+args.music)
                fx('fxloop 1');fx('fxparams 1 65536 32768 0')
                time.sleep(1)
                a=state();assert f'output_hz={args.rate} ' in a,a
                assert 'playing=0x1' in a,a
                c.command('hw audio state')
                time.sleep(7)
                a=state();assert f'output_hz={args.rate} ' in a,a
                assert 'playing=0x1' in a,a
                time.sleep(.317);b=state();assert phase(a)!=phase(b),(a,b)
                c.command('hw audio state')
                print(f'PASS: {args.rate} Hz source ended, effect phase continues over silence at that output rate',flush=True)
            finally:
                c.command('hw audio stop');c.command('hw audio fxfree 1')


if __name__=='__main__':
    main()
