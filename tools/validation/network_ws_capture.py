#!/usr/bin/env python3
"""N5.1 CLI gate using network_ws_peer.cjs. Exclusive UART; no APP API claim.

Logs/results stay outside Git. Match board and peer LAN first. Caller stops
peer/removes firewall rule/restores daily firmware after hardware validation.
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
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',required=True)
    p.add_argument('--host',required=True)
    p.add_argument('--sdk-tools',type=Path,required=True)
    p.add_argument('--ca',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    sys.path.insert(0,str(a.sdk_tools.resolve()))
    from aster_send import open_serial,send_file
    results={'status':'running','cases':[],'scope':'Firmware CLI, not SDK/APP or physical display acceptance'}
    handles=set();http_handles=set();ca_set=False
    with open_serial(a.port,timeout=.03,write_timeout=10,exclusive=True) as ser,(a.output/'serial.txt').open('w') as log:
        c=Client(ser,log)
        def command(text):
            end=time.monotonic()+12
            while True:
                value=c.command(text);print(value,flush=True)
                # Getter/admission/close may contend with a bounded TLS turn.
                # EAGAIN does not accept a side effect; retry a later event.
                if not re.search(r'^WS result=-11\r?$',value,re.M) or time.monotonic()>=end:
                    return value
                time.sleep(.1)
        def ok(text):
            value=command(text);assert re.search(r'^WS result=0\r?$',value,re.M),value
        def snap(h):
            v=command(f'aster net ws status {h}')
            assert re.search(r'^WS handle=\d+ state=',v,re.M),v
            return {k:int(n) for k,n in re.findall(r'(\w+)=(-?\d+)',v)}
        def wait(h,test,seconds=20):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                s=snap(h)
                if test(s):return s
                time.sleep(.15)
            raise AssertionError('WS state deadline: '+str(s))
        def start(path='/echo',tls=False,port=None,timeout=15000):
            value=command(f"aster net ws open {'wss' if tls else 'ws'}://{a.host}:{port or (17881 if tls else 17880)}{path} {timeout}")
            m=re.search(r'^WS handle=(\d+)',value,re.M);assert m,value
            h=int(m[1]);handles.add(h);return h
        def close(h):
            ok(f'aster net ws close {h}');handles.discard(h)
            until=time.monotonic()+8
            while time.monotonic()<until:
                s=command('aster net ws stats')
                if 'active=0 closing=0 tls=0 psram=0' in s:return
                time.sleep(.1)
            raise AssertionError('WS did not release')
        def receive(h,capacity=4096):
            end=time.monotonic()+12
            while True:
                value=command(f'aster net ws recv {h} {capacity}')
                if not re.search(r'^WS handle=\d+ result=-11 ',value,re.M) or time.monotonic()>=end:
                    break
                time.sleep(.1)
            m=re.search(r'^WS handle=\d+ result=(-?\d+) opcode=(\d+) required=(\d+)',value,re.M);assert m,value
            rc,op,n=map(int,m.groups())
            data=b''.join(bytes.fromhex(x) for x in re.findall(r'^WS data offset=\d+ hex=([0-9a-f]+)',value,re.M))
            if rc==0:assert len(data)==n and f'crc32={zlib.crc32(data):08x}' in value,value
            return rc,op,n,data
        def record(name,**extra):results['cases'].append({'name':name,'passed':True,**extra});print('WS_CASE_PASS',name,flush=True)
        try:
            for text in ['aster sys info','hw wifi status','aster net ws stats','aster sys mem']:
                command(text)
            assert 'active=0 closing=0 tls=0 psram=0' in command('aster net ws stats')
            assert send_file(a.port,a.ca,'/SD:/n5-ca.pem',connection=ser)=='OK'
            assert 'HTTP result=0' in command('aster net http ca /SD:/n5-ca.pem')
            ca_set=True
            for tls in (False,True):
                h=start(tls=tls);wait(h,lambda s:s['state']==4)
                for op,data in [(1,'你好 WebSocket'.encode()),(2,bytes(range(32))),(1,b''),(2,b'')]:
                    ok(f"aster net ws send {h} {op} {data.hex() or '-'}")
                    wait(h,lambda s:s['queued']==1)
                    rc,kind,n,value=receive(h);assert rc==0 and kind==op and value==data
                ok(f'aster net ws pattern {h} 4096 17');wait(h,lambda s:s['queued']==1)
                rc,op,n,data=receive(h,17);assert rc<0 and op==2 and n==4096
                rc,op,n,data=receive(h);assert rc==0 and data==bytes((i+17)%251 for i in range(4096))
                command('aster sys mem');command('aster net ws stats');command('netpool')
                close(h);record('wss_echo' if tls else 'ws_echo',max_message_bytes=4096)
            h=start(tls=True);wait(h,lambda s:s['state']==4)
            denial=command(f'aster net http start GET https://{a.host}:17881/fixed?n=32 15000 1048576')
            assert re.search(r'^HTTP result=-(16|28)\r?$',denial,re.M),denial
            assert 'HTTP result=-16' in command('aster net http ca default')
            hh=c.start(f'http://{a.host}:17880/fixed?n=16384');http_handles.add(hh)
            denial=command('aster net udp open');assert 'NET result=-28' in denial,denial
            ok(f'aster net ws pattern {h} 4096 19');wait(h,lambda s:s['queued']==1)
            assert receive(h)[3]==bytes((i+19)%251 for i in range(4096))
            command('aster sys mem');command('aster net ws stats');command('aster net http stats');command('netpool')
            body=bytearray();until=time.monotonic()+30
            while time.monotonic()<until:
                rc,chunk=c.read(hh)
                if rc>0:body.extend(chunk)
                elif rc==0:break
                elif rc!=-11:raise AssertionError(('HTTP concurrent',rc))
                time.sleep(.03)
            assert body==bytes(i%251 for i in range(16384)),len(body)
            c.command_ok(f'aster net http close {hh}');http_handles.discard(hh)
            close(h);record('wss_http_shared_admission_and_content',http_bytes=len(body))
            h=start('/fragment');s=wait(h,lambda s:s['queued']==1)
            assert s['pings']>=1
            assert receive(h)[3]=='中'.encode();close(h);record('fragment_control_interleaving')
            for path,error in [('/badaccept',None),('/oversize',None),('/badutf8',None),('/masked',None)]:
                h=start(path);s=wait(h,lambda s:s['state']==7);assert s['error']<0;saved=s['error'];close(h);record(path,error=saved)
            for port in (17882,17883):
                h=start(tls=True,port=port);s=wait(h,lambda s:s['state']==7);assert s['verify']!=0;close(h);record('certificate_rejection',port=port,verify=s['verify'],error=s['error'])
            for tls,port in [(False,17884),(True,17885)]:
                h=start(tls=tls,port=port,timeout=1000);s=wait(h,lambda s:s['state']==7);assert s['error']<0;close(h);record('stalled_handshake',port=port,error=s['error'])
            h=start('/close');s=wait(h,lambda s:s['state']==6);assert s['close_code']==1000;close(h);record('peer_close_handshake')
            h=start();wait(h,lambda s:s['state']==4)
            s=wait(h,lambda s:s['pongs']>=1,seconds=35);assert s['state']==4;close(h);record('idle_ping_pong')
            h=start('/stall');ok(f'aster net ws cancel {h}');s=wait(h,lambda s:s['state']==8);close(h);record('cancel_in_upgrade',error=s['error'])
            command('aster sys mem');command('aster net stats');command('netpool');command('net mem');command('kernel thread stacks')
            results['status']='passed'
        except BaseException as error:
            results.update(status='failed',error=str(error));raise
        finally:
            for h in list(handles):
                try:
                    command(f'aster net ws cancel {h}');close(h)
                except Exception as error:log.write('\nCLEANUP_FAILED '+repr(error)+'\n')
            for hh in http_handles:
                try:c.command_ok(f'aster net http close {hh}')
                except Exception as error:log.write('HTTP_CLEANUP_FAILED '+repr(error)+'\n')
            if ca_set:
                try:
                    assert 'HTTP result=0' in command('aster net http ca default')
                    assert 'removed' in command('aster fs rm /SD:/n5-ca.pem')
                except Exception as error:
                    results.update(status='failed',cleanup_error=str(error))
            (a.output/'result.json').write_text(json.dumps(results,indent=2)+'\n')
    assert results['status']=='passed',results
    print('WS_CLI_PASS',flush=True)


if __name__=='__main__':main()
