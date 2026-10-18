/* SPDX-License-Identifier: MIT. Local N5.1 protocol fixture; never a product service.
 * ASTER_WS_MODULE points to the installed ws package outside this repository.
 * Native Windows Node is used for WSL -> board LAN testing.
 * node network_ws_peer.cjs CERT_DIR; TCP 17880..17885. Stop after validation.
 */
'use strict';
const fs=require('node:fs'),path=require('node:path'),http=require('node:http'),https=require('node:https'),net=require('node:net');
const {WebSocketServer}=require(process.env.ASTER_WS_MODULE||'ws');
const dir=process.argv[2];if(!dir)throw Error('CERT_DIR required');
const stats={opened:0,closed:0,messages:0,bytes:0,pings:0,pongs:0};
const sockets=new Set();
function request(req,res){
 const u=new URL(req.url,'http://local');
 if(u.pathname==='/stats'){res.setHeader('Content-Type','application/json');res.end(JSON.stringify(stats));return;}
 if(u.pathname==='/stall')return;
 if(u.pathname==='/fixed'){const n=Math.min(65536,Math.max(0,Number(u.searchParams.get('n')||16384)));const b=Buffer.alloc(n);for(let i=0;i<n;i++)b[i]=i%251;res.end(b);return;}
 res.writeHead(404);res.end();
}
const wss=new WebSocketServer({noServer:true,maxPayload:4096,perMessageDeflate:false});
wss.on('connection',(socket,req)=>{
 stats.opened++;socket.on('close',()=>stats.closed++);socket.on('error',e=>console.log('WS_PEER_CLIENT_ERROR',e.code||e.message));
 socket.on('ping',()=>stats.pings++);
 socket.on('message',(data,binary)=>{stats.messages++;stats.bytes+=data.length;socket.send(data,{binary});});
 const p=new URL(req.url,'http://local').pathname;
 if(p==='/fragment'){
  socket.send(Buffer.from([0xe4,0xb8]),{binary:false,fin:false});socket.ping(Buffer.from('P'));socket.send(Buffer.from([0xad]),{binary:false,fin:true});
 }else if(p==='/slow'){
  /* Bounded bytewise frame: payload stays <= ASTER_WS_MESSAGE_MAX, so a slow
   * peer delays but can never extend one frame indefinitely. */
  const payload=Buffer.from([0x11,0x22,0x33,0x44,0x55,0x66]);
  socket._socket.write(Buffer.from([0x82,payload.length]));
  let i=0;const t=setInterval(()=>{
   if(i<payload.length)socket._socket.write(payload.subarray(i,++i));else clearInterval(t);
  },250);
  socket.on('close',()=>clearInterval(t));
 }else if(p==='/slowfrag'){
  /* First text fragment then silence: the client's 30 s assembly deadline
   * must fire even though the transport itself stays alive (ws auto-pong). */
  socket._socket.write(Buffer.from([0x01,0x04,0x61,0x62,0x63,0x64]));
 }else if(p==='/multi'){
  /* Ordered mixed batch incl. an empty binary message: a slow consumer must
   * still see every message exactly once, in order. */
  socket.send(Buffer.from([0xa1]),{binary:true});
  socket.send(Buffer.from([0xb1,0xb2]),{binary:true});
  socket.send(Buffer.alloc(0),{binary:true});
  socket.send(Buffer.from([0xc1,0xc2,0xc3]),{binary:true});
  socket.send('尾帧',{binary:false});
 }else if(p==='/ping'){
  /* Control frames interleaved with data. The single message slot makes an
   * unread data frame block later frames (bounded head-of-line), so the
   * client must consume d1..d3 before ping3 is reached and auto-ponged.
   * Once all three client pongs arrive the server confirms with a final
   * marker message: closed-loop proof of automatic pong responses. */
  let clientPongs=0;
  socket.on('pong',()=>{stats.pongs++;if(++clientPongs===3)socket.send(Buffer.from([3]),{binary:true});});
  socket.ping(Buffer.from('1'));socket.send(Buffer.from([0xd1]),{binary:true});
  socket.ping(Buffer.from('2'));socket.send(Buffer.from([0xd2]),{binary:true});
  socket.ping(Buffer.from('3'));socket.send(Buffer.from([0xd3]),{binary:true});
 }else if(p==='/blackhole'){
  /* Accept everything, echo nothing, stop reading: the TCP window fills and
   * the client's bounded output flush must time out (10 s output deadline,
   * 15 s idle ping + 10 s pong wait) instead of growing buffers forever. */
  socket._socket.pause();
 }else if(p==='/oversize')socket._socket.write(Buffer.from([0x82,126,0x10,1]));
 else if(p==='/badutf8')socket._socket.write(Buffer.from([0x81,2,0xc0,0x80]));
 else if(p==='/masked')socket._socket.write(Buffer.from([0x82,0x80,0,0,0,0]));
 else if(p==='/close')socket.close(1000);
});
function watch(server){server.on('connection',s=>{sockets.add(s);s.on('close',()=>sockets.delete(s));s.on('error',()=>{});});server.on('error',e=>{console.error(e);process.exit(1);});return server;}
function upgrade(server){
 server.on('upgrade',(req,socket,head)=>{
  const p=new URL(req.url,'http://local').pathname;
  if(p==='/badaccept'){socket.end('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: invalid\r\n\r\n');return;}
  if(p==='/stall')return;
  wss.handleUpgrade(req,socket,head,ws=>wss.emit('connection',ws,req));
 });return server;
}
upgrade(watch(http.createServer(request))).listen(17880,'0.0.0.0',()=>console.log('WS :17880'));
for(const [kind,port] of [['server',17881],['wrong',17882],['expired',17883]]){
 const opts={key:fs.readFileSync(path.join(dir,kind+'.key')),cert:fs.readFileSync(path.join(dir,kind+'.pem'))};
 upgrade(watch(https.createServer(opts,request))).listen(port,'0.0.0.0',()=>console.log('WSS',kind,port));
}
for(const port of [17884,17885])watch(net.createServer()).listen(port,'0.0.0.0',()=>console.log('STALL',port));
process.on('SIGINT',()=>{for(const s of sockets)s.destroy();process.exit(0);});
