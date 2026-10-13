#!/usr/bin/env python3
"""Optional physical AOT compatibility check, preserving installed APP data."""
import argparse, hashlib, json, sys, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sdk',type=Path,required=True);p.add_argument('--port',required=True);p.add_argument('--quickstart',type=Path,required=True);p.add_argument('--firmware-sha256',required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
sys.path.insert(0,str(a.sdk.resolve()/'tools'))
from aster_send import open_serial,send_file
from deploy import shell_command
import aster_board
out=a.output;out.mkdir(parents=True,exist_ok=True);log=[];results={'firmware_sha256':a.firmware_sha256,'firmware_hash_source':'Caller-supplied last flashed image hash; not read back by this test','cases':[]}
def save():
 (out/'hardware.log').write_text('\n'.join(log));(out/'hardware.json').write_text(json.dumps(results,indent=2)+'\n')
def sh(cmd):
 r=shell_command(a.port,cmd,timeout=45,connection=c);log.append('> '+cmd+'\n'+r);save();return r
with open_serial(a.port,timeout=.2,write_timeout=10) as c:
 info=sh('aster sys info');assert 'sd_mounted=yes' in info;results['device']=info
 listing=sh('aster app list');assert 'test.quickstart' not in listing,'Do not overwrite an existing user quickstart APP'
 for ident in ['aster.hello','aster.game.2048','test.quickstart']:
  pkg=a.quickstart if ident=='test.quickstart' else ROOT/'tests/fixtures/compatibility'/f'{ident}.ast'
  remote='/SD:/p1-'+ident+'.ast';assert send_file(a.port,pkg,remote,target=aster_board.get('xtensa'),connection=c)=='OK'
  assert 'installed OK' in sh('aster app install '+remote)
  assert 'wasm + aot' in sh('aster app info '+ident)
  reply=sh('aster app start '+ident);assert ' KB, AOT)' in reply,reply
  time.sleep(1);state=sh('aster app state '+ident)
  if ident=='aster.hello':assert 'Frame received; application lifecycle passed.' in reply and 'STOPPED' in state
  else:assert 'RUNNING' in state;sh('aster app stop '+ident)
  results['cases'].append({'id':ident,'sha256':hashlib.sha256(pkg.read_bytes()).hexdigest(),'aot_started':True,'state':state});save()
 sh('aster app uninstall test.quickstart')
 for ident in ['aster.hello','aster.game.2048','test.quickstart']:sh('aster fs rm /SD:/p1-'+ident+'.ast')
 listing=sh('aster app list');assert 'test.quickstart' not in listing;results['test_app_removed']=True;results['original_app_listing']=listing;results['status']='passed';save()
print('PASS frozen old AOT and documented quickstart deployment')
