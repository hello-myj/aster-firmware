#!/usr/bin/env python3
"""Compare existing graphics API submissions on one physical board; no API change."""
import argparse,hashlib,json,re,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sdk',type=Path,required=True);p.add_argument('--port',required=True);p.add_argument('--firmware',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--runs',type=int,default=3);p.add_argument('--resume',action='store_true');a=p.parse_args()
sys.path.insert(0,str(a.sdk.resolve()/'tools'))
from aster_send import open_serial,send_file
from deploy import shell_command
from screenshot import capture,png_bytes
import aster_board
out=a.output;out.mkdir(parents=True,exist_ok=True)
results={'package_sha256':{mode:hashlib.sha256((a.sdk.resolve()/'dist/xtensa'/('test.p2.gfx.'+mode+'.ast')).read_bytes()).hexdigest() for mode in ['individual','batched']},'config_sha256':hashlib.sha256((a.firmware.parent/'.config').read_bytes()).hexdigest(),'firmware_sha256':hashlib.sha256(a.firmware.read_bytes()).hexdigest(),'scenario':'RGB565 full clear + sixteen fixed rectangles; ordinary surface; 128 frames','runs':[]};log=[];c=None
def save():
 (out/'graphics.json').write_text(json.dumps(results,indent=2)+'\n');(out/'graphics.log').write_text('\n'.join(log))
def connect():return open_serial(a.port,timeout=.2,write_timeout=10)
def sh(cmd):
 r=shell_command(a.port,cmd,timeout=45,connection=c);log.append('> '+cmd+'\n'+r);save();return r
c=connect();results['device']=sh('aster sys info');assert 'sd_mounted=yes' in results['device']
listing=sh('aster app list')
for mode in ['individual','batched']:
 ident='test.p2.gfx.'+mode
 if ident in listing:
  assert a.resume,'Do not overwrite existing user benchmark APP'
  sh('aster app stop '+ident);continue
 pkg=a.sdk.resolve()/'dist/xtensa'/f'{ident}.ast'
 assert send_file(a.port,pkg,'/SD:/'+ident+'.ast',target=aster_board.get('xtensa'),connection=c)=='OK'
 assert 'installed OK' in sh('aster app install /SD:/'+ident+'.ast')
for run in range(a.runs):
 for mode in ['individual','batched']:
  ident='test.p2.gfx.'+mode;before=sh('aster sys mem')
  reply=sh('aster app start '+ident);assert ' KB, AOT)' in reply,reply
  deadline=time.monotonic()+120
  while 'GFXBENCH DONE' not in reply and time.monotonic()<deadline:reply+=c.read(4096).decode(errors='replace')
  # Drain delayed profiler snapshots after the APP finished drawing.
  until=time.monotonic()+.5
  while time.monotonic()<until:reply+=c.read(4096).decode(errors='replace')
  log.append(reply);save()
  m=re.search(r'GFXBENCH DONE mode=(\w+) frames=(\d+) draw_total_ms=(\d+) draw_max_ms=(\d+) interval_total_ms=(\d+) intervals=127 gfx_calls=(\d+) errors=(\d+)',reply)
  assert m and int(m[7])==0,reply
  stats=sh('aster app stats '+ident);active=sh('aster sys mem')
  c.close();c=None
  w,h,pixels=capture(a.port);(out/f'{mode}-{run}.png').write_bytes(png_bytes(w,h,pixels))
  c=connect();sh('aster app stop '+ident);time.sleep(.3);after=sh('aster sys mem')
  timing=re.findall(r'gfx tim: raster=(\d+) us flush=(\d+) us sum=(\d+) us \(max (\d+)/(\d+)\) cmds=(\d+) flushed=(\d+) px',reply)
  item={'run':run,'mode':mode,'frames':int(m[2]),'draw_mean_ms':int(m[3])/int(m[2]),'draw_max_ms':int(m[4]),'interval_mean_ms':int(m[5])/127,'gfx_calls':int(m[6]),'steady_frame_abi_calls':int(m[6])+2*int(m[2]),'steady_frame_abi_scope':'gfx_exec + two time_now_ms per frame; excludes init/log/stop','guest_linear_peak_bytes':int(re.search(r'peak=(\d+)',stats)[1]),'errors':int(m[7]),'screenshot_rgb565_sha256':hashlib.sha256(pixels).hexdigest(),'resolution':[w,h],'native_per_call_samples':[[int(n) for n in row] for row in timing],'memory_before':before,'memory_active':active,'memory_after':after,'app_stats':stats}
  results['runs'].append(item);save();print(f'PASS {run} {mode}: draw={item["draw_mean_ms"]:.2f}ms interval={item["interval_mean_ms"]:.2f}ms calls={item["gfx_calls"]}',flush=True)
assert len({r['screenshot_rgb565_sha256'] for r in results['runs']})==1,'Graphics optimization changed actual frame pixels'
for mode in ['individual','batched']:
 sh('aster app uninstall test.p2.gfx.'+mode);sh('aster fs rm /SD:/test.p2.gfx.'+mode+'.ast')
results['identical_pixels']=True;results['test_apps_removed']=True;results['status']='passed';save();c.close();print('PASS identical physical pixels across all runs',flush=True)
