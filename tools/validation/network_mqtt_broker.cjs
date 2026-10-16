/* SPDX-License-Identifier: MIT
 * N3 independent Aedes MQTT 3.1.1 peer (Windows/Linux).
 * Install aedes@0.51.3 outside Git. NODE_PATH points to that node_modules.
 * node network_mqtt_broker.cjs CERT_DIRECTORY
 * MQTT:1883; MQTTS valid:8883/wrong-name:8884/expired:8885;
 * HTTP:9080; loopback-only test control:9183. No credentials in logs.
 */
'use strict';
const aedes = require('aedes')(), fs = require('fs'), path = require('path');
const net = require('net'), tls = require('tls'), http = require('http');
const dir = process.argv[2];
if (!dir) throw Error('CERT_DIRECTORY required');
const peers = new Map();
let counts = { connections:0, disconnected:0, published:0, received:0, wills:0 };
const records = [];
aedes.authenticate = (client, username, password, cb) => {
  if (client.id.startsWith('deny-')) {
    const e = Error('not authorized'); e.returnCode = 5; return cb(e, false);
  }
  if (client.id.startsWith('auth-') &&
      (username !== 'fixture' || !password || password.toString() !== 'local-test-only')) {
    const e = Error('bad fixture credentials'); e.returnCode = 4; return cb(e, false);
  }
  cb(null, true);
};
aedes.authorizeSubscribe = (client, sub, cb) => cb(null,
  sub.topic.startsWith('denied/') ? null : sub);
aedes.on('clientReady', client => {
  peers.set(client.id, client); ++counts.connections;
  console.log(JSON.stringify({event:'ready',client:client.id}));
});
aedes.on('clientDisconnect', client => {
  peers.delete(client.id); ++counts.disconnected;
});
aedes.on('publish', (packet, client) => {
  if (packet.topic.startsWith('$SYS/')) return;
  ++counts.published;
  if (client) ++counts.received;
  if (packet.topic.startsWith('will/')) ++counts.wills;
  // Evidence contains only test topic/hex payload, never CONNECT credentials.
  records.push({topic:packet.topic,hex:packet.payload.toString('hex'),qos:packet.qos,retain:packet.retain});
  if (records.length > 256) records.shift();
});
aedes.on('clientError', () => {});
aedes.on('connectionError', () => {});
function track(server, port) {
  server.on('error', e => { console.error(e.message); process.exitCode=1; });
  server.listen(port,'0.0.0.0',()=>console.log(JSON.stringify({event:'listen',port})));
  return server;
}
const servers=[track(net.createServer(aedes.handle),1883)];
// Controlled peers that accept TCP but never answer MQTT/TLS.
for (const port of [1884,8886]) servers.push(track(net.createServer(socket=>{socket.setTimeout(30000,()=>socket.destroy());}),port));
for (const [name,port] of [['server',8883],['wrong',8884],['expired',8885]]) {
  servers.push(track(tls.createServer({key:fs.readFileSync(path.join(dir,name+'.key')),
    cert:fs.readFileSync(path.join(dir,name+'.pem')), minVersion:'TLSv1.2', maxVersion:'TLSv1.2'},aedes.handle),port));
}
servers.push(track(http.createServer((req,res)=>{
  const data=Buffer.from('aster-mqtt-http-coexist');
  res.writeHead(200,{'Content-Length':data.length});res.end(data);
}),9080));
const control=http.createServer((req,res)=>{
  let body='';req.on('data',b=>{body+=b;if(body.length>8192)req.destroy();});
  req.on('end',()=>{
    const reply=data=>{res.setHeader('Content-Type','application/json');res.end(JSON.stringify(data));};
    try {
      const o=body?JSON.parse(body):{};
      if(req.url==='/stats')return reply({...counts,clients:[...peers.keys()],records});
      if(req.url==='/reset'){records.length=0;counts={connections:0,disconnected:0,published:0,received:0,wills:0};return reply({ok:true});}
      if(req.url==='/drop') {
        const client=peers.get(o.client);if(client)client.conn.destroy();return reply({found:!!client});
      }
      if(req.url==='/publish') {
        if(typeof o.topic!=='string'||o.topic.length>256||typeof o.hex!=='string'||o.hex.length>4096)throw Error('bounds');
        return aedes.publish({cmd:'publish',topic:o.topic,payload:Buffer.from(o.hex,'hex'),qos:o.qos||0,retain:!!o.retain},e=>reply({ok:!e}));
      }
      res.statusCode=404;reply({error:'unknown test operation'});
    } catch {res.statusCode=400;reply({error:'invalid test request'});}
  });
});
control.listen(9183,'127.0.0.1',()=>console.log(JSON.stringify({event:'control',port:9183})));
function stop(){control.close();for(const s of servers)s.close();aedes.close(()=>process.exit());}
process.on('SIGINT',stop);process.on('SIGTERM',stop);
