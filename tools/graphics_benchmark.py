#!/usr/bin/env python3
"""Measure selected deterministic graphics scenes on one physical board."""
import argparse
import hashlib
import json
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODES = ('individual', 'batched', 'text', 'smallring', 'image', 'partial', 'frame')
EQUIVALENT = (('individual', 'batched'), ('text', 'smallring'), ('text', 'frame'))


def parse_done(reply, mode):
    match = re.search(r'GFXBENCH DONE mode=(\w+) frames=(\d+) draw_total_ms=(\d+) draw_max_ms=(\d+) interval_total_ms=(\d+) intervals=(\d+) gfx_calls=(\d+) errors=(\d+)', reply)
    if not match or match[1] != mode:
        raise ValueError('Missing or wrong benchmark completion record')
    frames, total, maximum, interval, intervals, calls, errors = map(int, match.groups()[1:])
    if frames < 2 or intervals != frames - 1 or errors:
        raise ValueError('Incomplete benchmark or APP errors')
    return {'frames': frames, 'draw_mean_ms': total / frames, 'draw_max_ms': maximum,
            'interval_mean_ms': interval / intervals, 'gfx_calls': calls, 'errors': errors,
            'steady_frame_abi_calls': calls + 2 * frames,
            'steady_frame_abi_scope': 'tracked gfx submissions/flushes + two time_now_ms per frame; excludes resource calls, libc imports and init/log/stop'}


def parse_profiler(reply):
    patterns = {
        'native_per_call_samples': r'gfx tim: raster=(\d+) us flush=(\d+) us sum=(\d+) us \(max (\d+)/(\d+)\) cmds=(\d+) flushed=(\d+) px',
        'direct_send_samples': r'disp tim: n=64 avg_rect=(\d+) B direct send \(no gather\) write=(\d+) us',
        'staged_samples': r'disp tim: n=64 avg_rect=(\d+) B staged gather=(\d+) us write=(\d+) us',
        'panel_bytes_per_flush': r'disp tim: n=64 avg_rect=(\d+) B avg_panel=(\d+) B staged gather=(\d+) us write=(\d+) us',
        'flip_samples': r'flip tim: acquire=(\d+) us copy\+sync=(\d+) us',
        'axs_transfer_samples': r'axs tim: n=64 avg_rect=(\d+) B avg_completed_panel=(\d+) B totals wait=(\d+) us gather=(\d+) us cache=(\d+) us write=(\d+) us errors=(\d+)',
    }
    result = {key: [[int(v) for v in row] for row in re.findall(pattern, reply)] for key, pattern in patterns.items()}
    result['axs_transfer_sample_columns'] = ['avg_rect_bytes', 'avg_completed_payload_bytes', 'wait_total_us', 'gather_total_us', 'cache_total_us', 'write_total_us', 'errors']
    result['axs_transfer_sample_scope'] = '64 transfer attempts; successful pixel payload only; excludes command headers; not 64 logical frames or optical FPS'
    return result


def verify_pixels(rows):
    # Each scene must repeat deterministically; only equivalent scenes match.
    for mode in {row['mode'] for row in rows}:
        if len({row['screenshot_rgb565_sha256'] for row in rows if row['mode'] == mode}) != 1:
            raise ValueError('Repeated scene pixels changed: ' + mode)
    checked = []
    for group in EQUIVALENT:
        selected = [row for row in rows if row['mode'] in group]
        if {row['mode'] for row in selected} == set(group):
            if len({row['screenshot_rgb565_sha256'] for row in selected}) != 1:
                raise ValueError('Equivalent scene pixels changed: ' + '/'.join(group))
            checked.append(list(group))
    return checked


def capture_checked(capture, port, on_retry):
    """Retry only incomplete transport; every accepted frame still passes CRC."""
    for attempt in range(3):
        try:
            return capture(port)
        except (TimeoutError, RuntimeError) as error:
            transport_error = isinstance(error, TimeoutError) or str(error).startswith(
                ('missing screenshot chunks:', 'screenshot length or CRC mismatch'))
            if not transport_error or attempt == 2:
                raise
            on_retry(attempt + 1, str(error))
            time.sleep(.5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('sdk', 'firmware', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--port', required=True)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--modes', default='individual,batched', help='Comma separated: ' + ','.join(MODES))
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    modes = args.modes.split(',')
    if args.runs < 1 or not modes or len(set(modes)) != len(modes) or any(mode not in MODES for mode in modes):
        parser.error('Use positive runs and unique supported modes')
    sys.path.insert(0, str(args.sdk.resolve() / 'tools'))
    from aster_send import open_serial, send_file
    from deploy import shell_command
    from screenshot import capture, png_bytes
    import aster_board

    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    results = {'package_sha256': {mode: hashlib.sha256((args.sdk.resolve() / 'dist/xtensa' / ('test.p2.gfx.' + mode + '.ast')).read_bytes()).hexdigest() for mode in modes},
               'config_sha256': hashlib.sha256((args.firmware.parent / '.config').read_bytes()).hexdigest(),
               'firmware_sha256': hashlib.sha256(args.firmware.read_bytes()).hexdigest(),
               'scenario': 'Selected deterministic RGB565 ordinary-surface scenes; 128 frames', 'modes': modes, 'runs': []}
    log = []
    connection = None
    def save():
        (out / 'graphics.json').write_text(json.dumps(results, indent=2) + '\n')
        (out / 'graphics.log').write_text('\n'.join(log))
    def connect():
        return open_serial(args.port, timeout=.2, write_timeout=10)
    def sh(command):
        reply = shell_command(args.port, command, timeout=45, connection=connection)
        log.append('> ' + command + '\n' + reply)
        save()
        return reply
    try:
        connection = connect()
        results['device'] = sh('aster sys info')
        assert 'sd_mounted=yes' in results['device']
        listing = sh('aster app list')
        existing = {line.split()[0]: line for line in listing.splitlines()
                    if re.search(r'\s(?:STOPPED|RUNNING|PAUSED|CRASHED)(?:\s|$)', line)}
        # A previous failure can leave the LAST mode running. Stop all selected
        # resumed fixtures before installing the FIRST package (installer is
        # globally busy while any APP is active).
        for mode in modes:
            ident = 'test.p2.gfx.' + mode
            if ident in existing:
                assert args.resume, 'Do not overwrite existing user benchmark APP'
                if re.search(r'\s(?:RUNNING|PAUSED)(?:\s|$)', existing[ident]):
                    reply = sh('aster app stop ' + ident)
                    assert 'failed:' not in reply, reply
        for mode in modes:
            ident = 'test.p2.gfx.' + mode
            # Resume always installs the measured local package, avoiding stale encoders.
            package = args.sdk.resolve() / 'dist/xtensa' / (ident + '.ast')
            assert send_file(args.port, package, '/SD:/' + ident + '.ast', target=aster_board.get('xtensa'), connection=connection) == 'OK'
            assert 'installed OK' in sh('aster app install /SD:/' + ident + '.ast')
        for run in range(args.runs):
            for mode in modes:
                ident = 'test.p2.gfx.' + mode
                before = sh('aster sys mem')
                reply = sh('aster app start ' + ident)
                assert ' KB, AOT)' in reply, reply
                deadline = time.monotonic() + 120
                while 'GFXBENCH DONE' not in reply and time.monotonic() < deadline:
                    reply += connection.read(4096).decode(errors='replace')
                until = time.monotonic() + .5
                while time.monotonic() < until:
                    reply += connection.read(4096).decode(errors='replace')
                log.append(reply)
                save()
                item = parse_done(reply, mode)
                stats = sh('aster app stats ' + ident)
                active = sh('aster sys mem')
                connection.close()
                connection = None
                def note_retry(attempt, error):
                    results.setdefault('screenshot_transport_retries', []).append(
                        {'run': run, 'mode': mode, 'attempt': attempt, 'error': error})
                    log.append('Screenshot transport retry: ' + error)
                    save()
                width, height, pixels = capture_checked(capture, args.port, note_retry)
                (out / f'{mode}-{run}.png').write_bytes(png_bytes(width, height, pixels))
                connection = connect()
                sh('aster app stop ' + ident)
                time.sleep(.3)
                after = sh('aster sys mem')
                item.update({'run': run, 'mode': mode, 'guest_linear_peak_bytes': int(re.search(r'peak=(\d+)', stats)[1]),
                             'screenshot_rgb565_sha256': hashlib.sha256(pixels).hexdigest(), 'resolution': [width, height],
                             'memory_before': before, 'memory_active': active, 'memory_after': after, 'app_stats': stats})
                item['screenshot_source_scope'] = ('guest RGB565 surface fallback; not downstream wire validation' if mode == 'partial' and 'board=aster.esp32s3.lckfb' in results['device'] else 'display record preferred; guest raster fallback if no display record')
                item.update(parse_profiler(reply))
                results['runs'].append(item)
                save()
                print(f'PASS {run} {mode}: draw={item["draw_mean_ms"]:.2f}ms interval={item["interval_mean_ms"]:.2f}ms calls={item["gfx_calls"]}', flush=True)
        results['pixel_equivalence_groups'] = verify_pixels(results['runs'])
        results['identical_pixels'] = len({row['screenshot_rgb565_sha256'] for row in results['runs']}) == 1
        results['scene_pixels_validated'] = True
        for mode in modes:
            sh('aster app uninstall test.p2.gfx.' + mode)
            sh('aster fs rm /SD:/test.p2.gfx.' + mode + '.ast')
        results['test_apps_removed'] = True
        results['status'] = 'passed'
        save()
    except Exception:
        results['status'] = 'failed'
        save()
        raise
    finally:
        if connection is not None:
            connection.close()
    print('PASS repeated scene pixels and selected equivalence groups', flush=True)


if __name__ == '__main__':
    main()
