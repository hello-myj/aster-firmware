#!/usr/bin/env python3
"""Summarize one N4 raw capture without mixing previous APP stop reports.

Writes bounded metrics JSON outside Git. Histograms use the APP's ms clock;
counts/throughput are workload results, never physical panel FPS/link bandwidth.
"""
import argparse
import json
from pathlib import Path
import re

ANSI = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    status = json.loads((a.capture/'result.json').read_text())
    text = ANSI.sub('', (a.capture/'serial.txt').read_text(errors='replace'))
    # An install can stop an older session before this run is armed. Scope to
    # the last successful init; preserve the original raw evidence unchanged.
    assert 'CONCUR INIT' in text, 'no workload init'
    text = text.rsplit('CONCUR INIT', 1)[1]
    rows = {}
    for match in re.finditer(r'\b(CONCUR|CONCURIO|CONCURPH|N4NET|N4LAT|N4UDP|N4RECOVER) ([^\r\n]*)', text):
        row = {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)', match[2])}
        rows.setdefault(match[1], []).append(row)
    last = {name:values[-1] for name,values in rows.items()}
    seconds = status.get('continuous_seconds', 0)
    metrics = {'capture_status':status['status'],'continuous_host_seconds':seconds,
               'gesture_cycles':status.get('gesture_cycles',0),'last':last}
    if seconds and 'N4NET' in last:
        metrics['validated_rx_average_bytes_per_second'] = round(last['N4NET']['bytes']/seconds, 2)
    if 'CONCURPH' in last:
        r = last['CONCURPH']
        metrics['FRAME_handler_wall_ms_per_phase'] = {
            str(i):round(r['t'+str(i)]/r['p'+str(i)],3) if r.get('p'+str(i)) else None
            for i in range(3)}
    if 'N4LAT' in last:
        latency = last['N4LAT']
        bins = [latency.get('b'+str(i),0) for i in range(8)]
        assert sum(bins) == latency['calls'], 'incomplete latency histogram'
        bounds = [0,1,3,7,15,31,63,None]
        quantiles = {}
        for name, fraction in [('p50',.5),('p95',.95),('p99',.99)]:
            total = 0
            for count, bound in zip(bins,bounds):
                total += count
                if total >= latency['calls']*fraction:
                    quantiles[name+'_bucket_upper_ms'] = bound
                    break
        metrics['SDK_ms_clock_histogram'] = quantiles
    def peak(pattern):
        values = [int(v) for v in re.findall(pattern,text)]
        return max(values) if values else None
    metrics['sampled_dram_used_max_bytes'] = peak(r'dram heap:[^\r\n]*used=(\d+)')
    metrics['sampled_libc_used_max_bytes'] = peak(r'libc heap:[^\r\n]*used=(\d+)')
    metrics['protocol_context_observed_max'] = peak(r'NETPOOL[^\r\n]*observed_max=(\d+)')
    metrics['TLS_pool_global_since_boot_peak_bytes'] = peak(r'crypto_peak=(\d+)')
    metrics['scope'] = [
        'TCP/HTTP completion max and MQTT ready max are different phase metrics.',
        'FRAME handler includes networking, enabled IMU, audio and rendering; not pure raster cost or physical FPS.',
        'Goodput is aggregate validated receive bytes divided by this mixed workload duration, not maximum link bandwidth.',
        'ms clock histogram has tick quantization; sampled pools are observed lower bounds.',
        'TLS, packet pool and stack high-water marks are global since boot and include earlier attempts.',
        'PSRAM static/pool reservation is not current APP allocation; coarse allocation probes cannot prove absence of small leaks.',
        'Disabled IMU and physical speaker output are not automatically verified.',
    ]
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(metrics,indent=2)+'\n')
    print(json.dumps(metrics,indent=2))


if __name__ == '__main__':
    main()
