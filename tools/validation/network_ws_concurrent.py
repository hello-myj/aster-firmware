#!/usr/bin/env python3
"""N5.1 firmware CLI Wi-Fi recovery + WSS/HTTP/foreground coexistence gate.

Keep the APP foreground until manual feedback. --stop-file stops only network
traffic and cleanup; restoring the original foreground/firmware is separate.
"""
import argparse
from pathlib import Path
import re
import sys
import time
from network_http_capture import Client


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',required=True);p.add_argument('--host',required=True)
    p.add_argument('--sdk-tools',type=Path,required=True);p.add_argument('--ca',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--app',required=True)
    p.add_argument('--seconds',type=int,default=600);p.add_argument('--stop-file',type=Path)
    p.add_argument('--wifi-recovery',action='store_true')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);sys.path.insert(0,str(a.sdk_tools.resolve()))
    from aster_send import open_serial,send_file
    handles=set();hh=None;disconnected=False;rounds=0;http_rounds=0
    with open_serial(a.port,timeout=.03,write_timeout=10,exclusive=True) as ser,(a.output/'serial.txt').open('w') as log:
        c=Client(ser,log)
        def cmd(s):
            deadline=time.monotonic()+12
            while True:
                r=c.command(s)
                if not re.search(r'^WS result=-11\r?$',r,re.M) or time.monotonic()>=deadline:break
                time.sleep(.1)
            print(r,flush=True);return r
        def ok(s):
            r=cmd(s);assert 'WS result=0' in r,r
        def snapshot(h):
            r=cmd(f'aster net ws status {h}');assert re.search(r'^WS handle=\d+ state=',r,re.M),r
            return {k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',r)}
        def wait(h,predicate,seconds=30):
            end=time.monotonic()+seconds
            while time.monotonic()<end:
                s=snapshot(h)
                if predicate(s):return s
                time.sleep(.1)
            raise AssertionError(s)
        def start():
            r=cmd(f'aster net ws open wss://{a.host}:17881/echo 15000');m=re.search(r'^WS handle=(\d+)',r,re.M);assert m,r
            h=int(m[1]);handles.add(h);wait(h,lambda s:s['state']==4);return h
        def close(h):
            ok(f'aster net ws close {h}');handles.discard(h)
            until=time.monotonic()+8
            while time.monotonic()<until:
                if 'active=0 closing=0 tls=0 psram=0' in cmd('aster net ws stats'):return
                time.sleep(.1)
            raise AssertionError('WS cleanup timeout')
        def echo(h,data):
            ok(f'aster net ws send {h} 2 {data.hex()}');wait(h,lambda s:s['queued']==1)
            until=time.monotonic()+12
            while time.monotonic()<until:
                r=cmd(f'aster net ws recv {h} 512')
                if not re.search(r'^WS handle=\d+ result=-11 ',r,re.M):break
                time.sleep(.1)
            assert re.search(r'^WS handle=\d+ result=0 opcode=2 ',r,re.M),r
            received=b''.join(bytes.fromhex(v) for v in re.findall(r'^WS data offset=\d+ hex=([0-9a-f]*)',r,re.M))
            assert received==data,r
        try:
            assert 'active=0 closing=0 tls=0 psram=0' in cmd('aster net ws stats')
            assert send_file(a.port,a.ca,'/SD:/n5-ca.pem',connection=ser)=='OK'
            assert 'HTTP result=0' in cmd('aster net http ca /SD:/n5-ca.pem')
            if a.wifi_recovery:
                h=start();echo(h,b'before-link-drop')
                cmd('hw wifi disconnect');disconnected=True
                s=wait(h,lambda s:s['state']==7,seconds=45);assert s['error']<0;close(h)
                cmd('hw wifi reconnect');end=time.monotonic()+70
                while time.monotonic()<end:
                    r=cmd('hw wifi status')
                    if 'state=COMPLETED' in r and re.search(r'ip=\d+\.\d+\.\d+\.\d+',r):break
                    time.sleep(1)
                else:raise AssertionError('saved Wi-Fi did not recover')
                disconnected=False;h=start();echo(h,b'after-link-restore');close(h)
                print('WS_WIFI_RECOVERY_PASS',flush=True)
            print(cmd(f'aster app start {a.app}'),flush=True)
            assert 'RUNNING' in cmd(f'aster app state {a.app}')
            h=start()
            hh=c.start(f'http://{a.host}:17880/fixed?n=16384',timeout=600000)
            body=bytearray()
            expected=bytes(i%251 for i in range(16384))
            end=time.monotonic()+a.seconds
            while time.monotonic()<end:
                if a.stop_file and a.stop_file.exists():break
                echo(h,f'ws-graph-{rounds:06d}'.encode());rounds+=1
                # Consume while WSS is active: leaving a full response queue
                # unread until manual feedback can expire a valid request.
                rc,data=c.read(hh)
                if rc>0:body.extend(data)
                elif rc==0:
                    assert body==expected,len(body)
                    http_rounds+=1
                    c.command_ok(f'aster net http close {hh}');hh=None
                    hh=c.start(f'http://{a.host}:17880/fixed?n=16384',timeout=600000)
                    body.clear()
                elif rc!=-11:raise AssertionError(rc)
                if rounds==1:
                    for s in ['aster net ws stats','aster net http stats','aster net stats','aster sys mem','netpool',f'aster app state {a.app}']:cmd(s)
                    print('WS_UI_READY',flush=True)
                time.sleep(.05)
            until=time.monotonic()+60
            while time.monotonic()<until:
                rc,data=c.read(hh)
                if rc>0:body.extend(data)
                elif rc==0:break
                elif rc!=-11:raise AssertionError(rc)
            assert body==expected,len(body)
            http_rounds+=1
            c.command_ok(f'aster net http close {hh}');hh=None;close(h)
            print(f'WS_UI_PASS rounds={rounds} http_rounds={http_rounds}',flush=True)
        finally:
            if disconnected:cmd('hw wifi reconnect')
            for h in list(handles):
                try:cmd(f'aster net ws cancel {h}');close(h)
                except Exception as error:print('WS_CLEANUP_FAILED',error,flush=True)
            if hh:c.command_ok(f'aster net http close {hh}')
            time.sleep(2)
            assert 'HTTP result=0' in cmd('aster net http ca default')
            cmd('aster fs rm /SD:/n5-ca.pem')
            for s in ['aster net ws stats','aster net http stats','aster net stats','aster sys mem','netpool','kernel thread stacks',f'aster app state {a.app}']:cmd(s)


if __name__=='__main__':main()
