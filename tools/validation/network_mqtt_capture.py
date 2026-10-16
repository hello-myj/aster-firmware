#!/usr/bin/env python3
"""N3 MQTT CLI gate against independent network_mqtt_broker.cjs.

Profiles/CA must be installed separately. UART exclusive; evidence outside Git.
For WSL+Windows broker use --windows-node: control stays on Windows loopback,
while the device talks directly to the PC's LAN address without the PC VPN.
"""
import argparse
import json
import re
import subprocess
import sys
import time
import urllib.request
import zlib
from pathlib import Path
from network_http_capture import Client

class Gate:
    def __init__(self, client, node=None):
        self.c, self.node, self.handles = client, node, set()
    def cmd(self, tail):
        end = time.monotonic() + 12
        while True:
            response = self.c.command('aster net mqtt ' + tail)
            # A TLS crypto slice may hold the nonblocking service lock.
            # EAGAIN is part of the contract, not protocol failure.
            if 'MQTT result=-11' not in response or time.monotonic() >= end:
                return response
            time.sleep(.12)
    def control(self, command, data=None):
        body = json.dumps(data or {})
        url = 'http://127.0.0.1:9183/' + command
        if self.node:
            script = "fetch(process.argv[1],{method:'POST',headers:{'Content-Type':'application/json'},body:process.argv[2]}).then(r=>r.text()).then(t=>process.stdout.write(t)).catch(e=>{console.error(e.message);process.exit(1)})"
            return json.loads(subprocess.check_output([self.node, '-e', script, url, body], text=True, timeout=10))
        request = urllib.request.Request(url, body.encode(), {'Content-Type': 'application/json'})
        return json.load(urllib.request.urlopen(request, timeout=10))
    def open(self, profile):
        response = self.cmd('connect /SD:/n3-' + profile + '.profile')
        match = re.search(r'^MQTT handle=(\d+)', response, re.M)
        assert match, response
        h = int(match[1]); self.handles.add(h); return h
    def status(self, h):
        response = self.cmd(f'result {h}')
        record = re.search(r'^MQTT handle=\d+ state=.*', response, re.M)
        assert record, response
        return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', record[0])}
    def wait(self, h, condition, seconds=25):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            s = self.status(h)
            if condition(s): return s
            time.sleep(.15)
        raise AssertionError(('MQTT state deadline', h, s))
    def ready(self, profile):
        h = self.open(profile)
        s = self.wait(h, lambda s: s['state'] == 4 or s['state'] >= 6)
        assert s['state'] == 4, s
        return h
    def op(self, command):
        response = self.cmd(command)
        match = re.search(r'^MQTT operation=(\d+)', response, re.M)
        assert match, response
        return int(match[1])
    def done(self, h, operation, error=0):
        s = self.wait(h, lambda s: s['last_operation'] == operation or s['state'] >= 6)
        assert s['last_operation'] == operation and s['operation_error'] == error, s
        response = self.cmd(f'operation {h} {operation}')
        assert f'MQTT operation={operation} error={error}' in response, response
    def receive(self, h, topic, data, retain=None):
        end = time.monotonic() + 8
        while time.monotonic() < end:
            response = self.cmd(f'recv {h}')
            match = re.search(r'^MQTT message topic=(.*?) length=(\d+) qos=(\d+) retain=(\d+) dup=(\d+) crc32=([0-9a-f]+)', response, re.M)
            if match:
                assert match[1] == topic and int(match[2]) == len(data), response
                received = b''.join(bytes.fromhex(x) for x in re.findall(r'^MQTT data offset=\d+ hex=([0-9a-f]+)', response, re.M))
                assert received == data and int(match[6], 16) == zlib.crc32(data), response
                if retain is not None: assert int(match[4]) == retain, response
                return
            time.sleep(.1)
        raise AssertionError(('missing MQTT message', h, topic))
    def close(self, h):
        response = self.cmd(f'close {h}'); assert 'MQTT result=0' in response, response
        self.handles.discard(h); time.sleep(1.8)
        response = self.cmd('stats'); assert 'active=0 closing=0 tls=0 psram=0' in response, response
    def run(self):
        self.control('reset')
        h = self.ready('plain')
        self.done(h, self.op(f'subscribe {h} aster/n3/# 1'))
        for qos, data in [(0, b'hello\x00mqtt'), (1, b'qos-one'), (0, b'')]:
            self.done(h, self.op(f'publish {h} aster/n3/echo {qos} 0 ' + (data.hex() or '-')))
            self.receive(h, 'aster/n3/echo', data)
        # Independent peer sends maximum-sized payload, exceeding TCP buffers
        # only through bounded framing, not a local SDK echo shortcut.
        data = bytes(i % 251 for i in range(1024))
        assert self.control('publish', {'topic':'aster/n3/max','hex':data.hex(),'qos':1})['ok']
        self.receive(h, 'aster/n3/max', data)
        # Burst exceeds the two-message queue. All three must survive slow APP
        # consumption without overwrite, with bounded parser/TCP storage.
        for i in range(3):
            assert self.control('publish', {'topic':'aster/n3/burst','hex':bytes([i]).hex(),'qos':1})['ok']
        assert self.status(h)['queued'] == 2
        for i in range(3): self.receive(h, 'aster/n3/burst', bytes([i]))
        self.done(h, self.op(f'subscribe {h} denied/n3 1'), error=-13)
        self.done(h, self.op(f'publish {h} aster/n3/retained 1 1 6b656570'))
        self.receive(h, 'aster/n3/retained', b'keep', retain=0)
        self.done(h, self.op(f'unsubscribe {h} aster/n3/#'))
        self.done(h, self.op(f'subscribe {h} aster/n3/# 1'))
        self.receive(h, 'aster/n3/retained', b'keep', retain=1)
        self.done(h, self.op(f'publish {h} aster/n3/retained 1 1 -'))
        self.receive(h, 'aster/n3/retained', b'', retain=0)
        self.done(h, self.op(f'unsubscribe {h} aster/n3/#'))
        # No APP polling is needed for keepalive.
        time.sleep(12)
        assert self.status(h)['state'] == 4
        self.close(h)
        h = self.ready('auth'); self.close(h)
        for profile, code in [('denied',5),('auth-bad',4)]:
            h=self.open(profile);s=self.wait(h,lambda s:s['state']>=6)
            assert s['state']==6 and s['connack']==code, s
            self.close(h)
        h=self.ready('reconnect')
        self.done(h,self.op(f'subscribe {h} aster/n3/reconnect 1'))
        assert self.control('drop',{'client':'aster-n3-reconnect'})['found']
        self.wait(h,lambda s:s['state']==4 and s['reconnects']>=1 and s['subscriptions']==1)
        assert self.control('publish',{'topic':'aster/n3/reconnect','hex':'726573746f726564','qos':1})['ok']
        self.receive(h,'aster/n3/reconnect',b'restored');self.close(h)
        # Graceful close suppresses Will; cancellation deliberately has no DISCONNECT.
        h=self.ready('will');self.close(h)
        assert not any(r['topic']=='will/aster-n3' for r in self.control('stats')['records'])
        h=self.ready('will');assert 'MQTT result=0' in self.cmd(f'cancel {h}')
        self.wait(h,lambda s:s['state']==7);time.sleep(1)
        assert any(r['topic']=='will/aster-n3' and r['hex']=='6f66666c696e65' for r in self.control('stats')['records'])
        self.close(h)
        # Existing default CA must reject the local private CA.
        h=self.open('tls');s=self.wait(h,lambda s:s['state']>=6)
        assert s['state']==6 and s['tls_verify'],s
        self.close(h)
        response=self.c.command('aster net http ca /SD:/n3-ca.pem');assert 'HTTP result=0' in response,response
        try:
            h=self.ready('tls');assert self.status(h)['tls_verify']==0
            response=self.cmd('connect /SD:/n3-tls.profile');assert 'MQTT result=' in response and 'MQTT handle=' not in response,response
            response=self.c.command('aster net http start GET https://192.168.110.76:8883/ 1000 100')
            assert 'HTTP result=' in response and 'HTTP handle=' not in response,response
            response=self.c.command('aster net http ca default');assert 'HTTP result=0' not in response,response
            self.done(h,self.op(f'subscribe {h} aster/n3/tls 1'))
            self.done(h,self.op(f'publish {h} aster/n3/tls 1 0 736563757265'))
            self.receive(h,'aster/n3/tls',b'secure');self.close(h)
            for profile in ['wrong','expired']:
                h=self.open(profile);s=self.wait(h,lambda s:s['state']>=6)
                assert s['state']==6 and s['tls_verify'],s
                self.close(h)
        finally:
            for h in list(self.handles):
                try:self.close(h)
                except Exception:pass
            self.c.command('aster net http ca default')
        print('MQTT CLI G1 basic gate PASS',flush=True)

    def advanced(self):
        h=self.ready('plain')
        self.done(h,self.op(f'subscribe {h} aster/n3/retain/# 1'))
        self.done(h,self.op(f'publish {h} aster/n3/retain/state 1 1 6b656570'))
        self.receive(h,'aster/n3/retain/state',b'keep',retain=0)
        self.done(h,self.op(f'unsubscribe {h} aster/n3/retain/#'))
        self.done(h,self.op(f'subscribe {h} aster/n3/retain/# 1'))
        self.receive(h,'aster/n3/retain/state',b'keep',retain=1)
        self.done(h,self.op(f'publish {h} aster/n3/retain/state 1 1 -'))
        self.receive(h,'aster/n3/retain/state',b'',retain=0)
        assert self.control('publish',{'topic':'aster/n3/retain/oversize','hex':(b'x'*1025).hex(),'qos':1})['ok']
        s=self.wait(h,lambda s:s['state']>=6)
        assert s['state']==6 and s['error'],s
        self.close(h)
        h=self.open('stall')
        s=self.wait(h,lambda s:s['state']>=6,seconds=8)
        assert s['state']==6 and s['error'],s
        self.close(h)
        response=self.c.command('aster net http ca /SD:/n3-ca.pem');assert 'HTTP result=0' in response,response
        try:
            h=self.open('tls-stall')
            self.wait(h,lambda s:s['state']==2)
            assert 'MQTT result=0' in self.cmd(f'cancel {h}')
            self.wait(h,lambda s:s['state']==7);self.close(h)
        finally:self.c.command('aster net http ca default')
        h=self.ready('reconnect')
        self.done(h,self.op(f'subscribe {h} aster/n3/wifi-recovery 1'))
        print(self.c.command('hw wifi disconnect'),flush=True)
        time.sleep(12)
        print(self.c.command('hw wifi reconnect'),flush=True)
        s=self.wait(h,lambda s:s['state']==4 and s['reconnects']>=1 and s['subscriptions']==1,seconds=70)
        assert self.control('publish',{'topic':'aster/n3/wifi-recovery','hex':'776966692d6f6b','qos':1})['ok']
        self.receive(h,'aster/n3/wifi-recovery',b'wifi-ok')
        self.close(h)
        print('MQTT advanced retain/oversize/timeout/cancel/Wi-Fi PASS',flush=True)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',required=True);p.add_argument('--sdk',type=Path,required=True)
    p.add_argument('--log',type=Path,required=True);p.add_argument('--windows-node')
    p.add_argument('--advanced-only',action='store_true')
    a=p.parse_args();sys.path.insert(0,str(a.sdk/'tools'))
    from aster_send import open_serial
    a.log.parent.mkdir(parents=True,exist_ok=True)
    with a.log.open('w') as log,open_serial(a.port,timeout=.1) as serial:
        gate=Gate(Client(serial,log),a.windows_node)
        try:gate.advanced() if a.advanced_only else gate.run()
        finally:
            for h in list(gate.handles):
                try:gate.close(h)
                except Exception:pass
if __name__=='__main__':main()
