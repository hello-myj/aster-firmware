/* SPDX-License-Identifier: MIT. N4 independent LAN peer coordinator.
 * NODE_PATH must contain aedes@0.51.3 outside Git. Certificates stay outside Git.
 * node network_n4_peer.cjs CERT_DIR
 * HTTP/HTTPS 9080/9443..9446, MQTT/MQTTS 1883/8883..8886,
 * TCP/UDP echo 17780/17781; control only on localhost:9183.
 */
'use strict';
const net=require('node:net'),dgram=require('node:dgram');
if(!process.argv[2])throw Error('CERT_DIR required');
process.env.ASTER_MQTT_HTTP_PORT='9081';
require('./network_mqtt_broker.cjs');
require('./network_http_server.cjs');
let active=0,accepted=0,completed=0;
const tcp=net.createServer(s=>{
 if(active>=4){console.log('N4TCP rejected',active);s.destroy();return;}active++;const id=++accepted;let count=0,ended=false;console.log('N4TCP accepted',id,s.remoteAddress,s.remotePort,'active',active);
 s.setTimeout(10000,()=>{console.log('N4TCP timeout',id,count);s.destroy();});s.on('error',e=>console.log('N4TCP error',id,e.code));s.on('close',()=>{active--;console.log('N4TCP closed',id,'bytes',count,'active',active);});
 s.on('data',b=>{if(ended)return;count+=b.length;for(let i=0;i<b.length;i+=17)s.write(b.subarray(i,i+17));if(count>=512){ended=true;completed++;s.end();}});
});
const udp=dgram.createSocket('udp4');
for(const s of [tcp,udp])s.on('error',e=>{console.error(e.message);process.exit(1);});
udp.on('message',(data,peer)=>{if(data.length<=1200)udp.send(data,peer.port,peer.address);});
tcp.listen(17780,'0.0.0.0',()=>console.log('N4 TCP :17780'));
udp.bind(17781,'0.0.0.0',()=>console.log('N4 UDP :17781'));
