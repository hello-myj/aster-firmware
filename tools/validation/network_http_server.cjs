/* SPDX-License-Identifier: MIT
 * Controlled HTTP/HTTPS peer for N2. Node builtins only, Windows/Linux.
 * Usage: node network_http_server.cjs [CERT_DIRECTORY]
 * Cert directory contains server.key/server.pem, wrong.key/wrong.pem,
 * expired.key/expired.pem. Private keys and generated evidence stay outside Git.
 */
const http = require('http'), https = require('https'), net = require('net');
const fs = require('fs'), path = require('path');
let requests = 0;
const sockets = new Set();
function track(server) {
  server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
  server.on('error', e => { console.error(e.message); process.exitCode = 1; });
  return server;
}
function handler(req, res) {
  ++requests;
  const url = new URL(req.url, 'http://fixture.invalid');
  const count = Math.min(1048576, Math.max(0, Number(url.searchParams.get('n') || 4096)));
  const pattern = Buffer.alloc(count);
  for (let i = 0; i < count; ++i) pattern[i] = i % 251;
  let received = [], size = 0;
  req.on('data', b => { size += b.length; if (size <= 8192) received.push(b); else req.destroy(); });
  req.on('error', () => {});
  req.on('end', () => {
    res.setHeader('Connection', 'close');
    res.setHeader('X-Repeat', ['one', 'two']);
    if (url.pathname === '/json') {
      const data = Buffer.from('{"ok":true,"value":42}\n');
      res.writeHead(200, {'Content-Type':'application/json','Content-Length':data.length});
      return res.end(data);
    }
    if (url.pathname === '/redirect') {
      res.writeHead(Number(url.searchParams.get('code') || 302), {Location: url.searchParams.get('to') || '/fixed?n=8', 'Content-Length': 0});
      return res.end();
    }
    if (url.pathname === '/echo' || url.pathname === '/headers') {
      const data = url.pathname === '/echo' ? Buffer.concat(received) : Buffer.from(JSON.stringify({method:req.method, headers:req.headers, body:Buffer.concat(received).toString()}));
      res.writeHead(200, {'Content-Length':data.length}); return res.end(data);
    }
    if (url.pathname === '/truncated') {
      res.writeHead(200, {'Content-Length':count+100});
      res.write(pattern); return setTimeout(() => res.destroy(), 50);
    }
    if (url.pathname === '/chunked') {
      res.writeHead(200, {'Trailer':'X-Complete'});
      for (let i = 0; i < count; i += 113) res.write(pattern.subarray(i, i+113));
      res.addTrailers({'X-Complete':'yes'}); return res.end();
    }
    if (url.pathname === '/eof') {
      res.useChunkedEncodingByDefault = false;
      res.writeHead(200); return res.end(pattern);
    }
    if (url.pathname === '/stall') return;
    const reply = () => {
      const status = url.pathname.startsWith('/status/') ? Number(url.pathname.slice(8)) : 200;
      res.writeHead(status, {'Content-Length':pattern.length});
      if (req.method === 'HEAD') return res.end();
      if (url.pathname !== '/slow') return res.end(pattern);
      let offset = 0;
      const timer = setInterval(() => {
        if (res.destroyed || offset >= pattern.length) { clearInterval(timer); if (!res.destroyed) res.end(); return; }
        res.write(pattern.subarray(offset, offset+64)); offset += 64;
      }, 100);
    };
    const delay = Math.min(30000, Math.max(0, Number(url.searchParams.get('delay') || 0)));
    if (delay) setTimeout(reply, delay); else reply();
  });
}
const servers = [track(http.createServer(handler)).listen(9080, '0.0.0.0'),
  track(net.createServer(() => {})).listen(9446, '0.0.0.0')];
const directory = process.argv[2];
if (directory) {
  for (const [name, port] of [['server',9443],['wrong',9444],['expired',9445]]) {
    const options = {key:fs.readFileSync(path.join(directory,name+'.key')),
      cert:fs.readFileSync(path.join(directory,name+'.pem')), minVersion:'TLSv1.2', maxVersion:'TLSv1.2'};
    const server = track(https.createServer(options, handler));
    server.on('tlsClientError', () => {}); servers.push(server.listen(port, '0.0.0.0'));
  }
}
setInterval(() => console.log(JSON.stringify({requests,sockets:sockets.size})), 60000).unref();
process.on('SIGINT', () => { for (const s of sockets) s.destroy(); for (const s of servers) s.close(); });
console.log(JSON.stringify({ready:true,http:9080,https:directory?[9443,9444,9445]:[],stall:9446}));
