#!/usr/bin/env python3
"""Short N3 CLI coexistence gate; resource evidence needs diagnostic config.
Keeps chosen APP in foreground; optional --stop-file ends traffic gracefully.
Original foreground/volume restoration is a separate explicit final step.
"""
import argparse
import re
import sys
import time
from pathlib import Path
from network_http_capture import Client
from network_mqtt_capture import Gate

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',required=True);p.add_argument('--sdk',type=Path,required=True)
    p.add_argument('--log',type=Path,required=True);p.add_argument('--app',required=True)
    p.add_argument('--host',required=True);p.add_argument('--seconds',type=int,default=90)
    p.add_argument('--stop-file',type=Path);p.add_argument('--diagnostic',action='store_true')
    p.add_argument('--quiet-ui',action='store_true',help='defer large diagnostic dumps until interaction is complete')
    a=p.parse_args();sys.path.insert(0,str(a.sdk/'tools'))
    from aster_send import open_serial
    a.log.parent.mkdir(exist_ok=True,parents=True)
    with a.log.open('w') as log,open_serial(a.port,timeout=.1) as serial:
        c=Client(serial,log);g=Gate(c);http_handle=None
        end=time.monotonic()+50
        while True:
            if 'state=COMPLETED' in c.command('hw wifi status'):break
            assert time.monotonic()<end,'Wi-Fi unavailable'
            time.sleep(2)
        for cmd in ['aster net mqtt stats','aster net http stats','aster sys mem']:
            print(c.command(cmd),flush=True)
        if a.diagnostic: print(c.command('netpool reset'),flush=True)
        print(c.command(f'aster app start {a.app}'),flush=True)
        assert 'RUNNING' in c.command(f'aster app state {a.app}')
        response=c.command('aster net http ca /SD:/n3-ca.pem');assert 'HTTP result=0' in response,response
        try:
            h=g.ready('tls')
            g.done(h,g.op(f'subscribe {h} aster/n3/concurrent 1'))
            # Plain HTTP occupies the other shared connection while MQTTS
            # owns TLS. Terminal HTTP retains quota until explicit close.
            for _ in range(20):
                response=c.command(f'aster net http start GET http://{a.host}:9080/ 15000 1024')
                m=re.search(r'HTTP handle=(\d+)',response)
                if m:http_handle=int(m[1]);break
                time.sleep(.15)
            assert http_handle,response
            end=time.monotonic()+a.seconds;rounds=0
            print('MQTT_UI_READY',flush=True)
            while time.monotonic()<end:
                if a.stop_file and a.stop_file.exists():break
                data=f'mqtt-graph-{rounds:04d}'.encode()
                g.done(h,g.op(f'publish {h} aster/n3/concurrent 1 0 {data.hex()}'))
                g.receive(h,'aster/n3/concurrent',data)
                rounds+=1
                if not a.quiet_ui and (rounds==1 or rounds%15==0):
                    for cmd in ['aster net mqtt stats','aster net http stats','aster net stats','aster sys mem']:
                        print(c.command(cmd),flush=True)
                    if a.diagnostic:
                        for cmd in ['netpool','net mem','kernel thread stacks']:
                            print(c.command(cmd),flush=True)
                time.sleep(.05)
            # Verify HTTP independently, not merely admission to its slot.
            text=c.command(f'aster net http read {http_handle} 128')
            received=b''.join(bytes.fromhex(x) for x in re.findall(r'HTTP data offset=\d+ hex=([0-9a-f]+)',text))
            assert received==b'aster-mqtt-http-coexist',text
            print(f'MQTT concurrent PASS rounds={rounds}',flush=True)
        finally:
            for h in list(g.handles):
                try:g.close(h)
                except Exception as e:print('MQTT cleanup:',e,flush=True)
            if http_handle:print(c.command(f'aster net http close {http_handle}'),flush=True)
            time.sleep(2)
            print(c.command('aster net http ca default'),flush=True)
            for cmd in ['aster net mqtt stats','aster net http stats','aster net stats','aster sys mem',f'aster app state {a.app}']:
                print(c.command(cmd),flush=True)
if __name__=='__main__':main()
