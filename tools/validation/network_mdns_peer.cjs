/* SPDX-License-Identifier: MIT. Local N5.2 mDNS test peer; never a product
 * service. Native Windows Node (dgram only). Binds 5353 with reuse and joins
 * 224.0.0.251 on the LAN adapter so queries and announcements traverse the
 * same path real phones use; --direct <device-ip> unicasts the query instead
 * for routers that drop local multicast.
 *   node network_mdns_peer.cjs query <name> [--direct IP] [--lan IP] [--wait MS]
 *   node network_mdns_peer.cjs answer <name> <ip> [--lan IP] [--seconds N]
 *       (answers queries for <name> as <ip>, logs received announces)
 *   node network_mdns_peer.cjs conflict <name> <ip> [--lan IP] [--seconds N]
 *       (unsolicited A answer for <name> claiming <ip>, repeatedly)
 * Machine-readable lines: MDNS_PEER_*. */
'use strict';
const dgram = require('node:dgram');
const Group = '224.0.0.251', Port = 5353;
const [mode, name, extra] = process.argv.slice(2);
if ((!name && mode !== 'observe') || ['query','query-ephemeral','answer','conflict','observe'].indexOf(mode) < 0)
  throw Error('usage: peer query|query-ephemeral|answer|conflict <name> [ip] [--direct IP] [--lan IP] [--wait MS] [--seconds N] | peer observe [--lan IP] [--seconds N]');
const arg = (k) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : null; };
const DIRECT = arg('--direct'), LAN = arg('--lan'), WAIT = Number(arg('--wait') || 3000),
      SECONDS = Number(arg('--seconds') || 12);
function encode(name) {
  const labels = name.split('.').map((l) => Buffer.from(l));
  for (const l of labels) if (!l.length || l.length > 63) throw Error('bad label');
  return Buffer.concat([labels.map((l) => Buffer.concat([Buffer.from([l.length]), l])).reduce((a, b) => Buffer.concat([a, b]), Buffer.alloc(0)), Buffer.from([0])]);
}
function queryPacket(name) {
  const q = encode(name);
  return Buffer.concat([Buffer.from([0, 0, 0, 0x01, 0, 1, 0, 0, 0, 0, 0, 0]), q, Buffer.from([0, 1, 0, 1])]);
}
/* follow one (possibly compressed) name; return {dotted, end} where end is the
 * wire offset just past the name in its ORIGINAL section, or null on overrun. */
function readName(buf, start, n) {
  const parts = []; let at = start, jumps = 0, end = -1;
  for (;;) {
    if (at >= n) return null;
    const l = buf[at];
    if (!l) { if (end < 0) end = at + 1; break; }
    if (l & 0xc0) {
      if (++jumps > 4 || at + 2 > n) return null;
      if (end < 0) end = at + 2;
      at = ((l & 63) << 8) | buf[at + 1];
      continue;
    }
    at++;
    if (at + l > n) return null;
    parts.push(buf.toString('ascii', at, at + l));
    at += l;
  }
  return { dotted: parts.join('.').toLowerCase(), end: end < 0 ? at : end };
}
function isAnswer(buf) { return buf.length >= 12 && (buf.readUInt16BE(2) & 0x8000) !== 0; }
function parseAnswer(buf) {
  const n = buf.length;
  if (n < 12) return null;
  if (!isAnswer(buf) || !buf.readUInt16BE(6)) return null;
  const q = readName(buf, 12, n);
  if (!q) return null;
  let at = q.end + 4; /* skip QTYPE/QCLASS */
  const an = buf.readUInt16BE(6);
  for (let i = 0; i < an; i++) {
    const r = readName(buf, at, n);
    if (!r) return null;
    at = r.end;
    if (at + 10 > n) return null;
    const type = buf.readUInt16BE(at), ttl = buf.readUInt32BE(at + 4), rd = buf.readUInt16BE(at + 8);
    at += 10;
    if (type === 1 && rd === 4 && at + 4 <= n)
      return { name: q.dotted, ip: `${buf[at]}.${buf[at + 1]}.${buf[at + 2]}.${buf[at + 3]}`, ttl };
    at += rd;
  }
  return null;
}
function questionName(buf) { const r = readName(buf, 12, buf.length); return r ? r.dotted : null; }
function questionType(buf) {
  const r = readName(buf, 12, buf.length);
  return r && r.end + 2 <= buf.length ? buf.readUInt16BE(r.end) : 0;
}
function questionTail(buf) { /* question name + QTYPE + QCLASS slice to echo */
  const r = readName(buf, 12, buf.length);
  return r && r.end + 4 <= buf.length ? buf.subarray(12, r.end + 4) : null;
}
function openSocket() {
  const s = dgram.createSocket({ type: 'udp4', reuseAddr: true });
  s.on('error', (e) => { console.log('MDNS_PEER_ERROR ' + e.code); });
  s.bind(Port, LAN || '0.0.0.0', () => { try { s.addMembership(Group, LAN || undefined); } catch (e) { console.log('MDNS_PEER_JOIN_WARN ' + e.message); } ready(s); });
  return s;
}
function ready(s) {
  if (mode === 'query') {
    const target = DIRECT || Group;
    const packet = queryPacket(name);
    let done = false;
    s.on('message', (msg, rinfo) => {
      const a = parseAnswer(msg);
      if (a && a.name === name.toLowerCase()) {
        console.log(`MDNS_PEER_ANSWER name=${name} ip=${a.ip} ttl=${a.ttl} from=${rinfo.address}`);
        done = true; s.close(); process.exit(0);
      }
    });
    const send = () => s.send(packet, 0, packet.length, Port, target, (e) => { if (e) { console.log('MDNS_PEER_ERROR ' + e.code); process.exit(2); } });
    send();
    setTimeout(() => send(), 400); /* mDNS questions are normally repeated */
    setTimeout(() => { if (!done) { console.log('MDNS_PEER_TIMEOUT name=' + name + ' target=' + target); s.close(); process.exit(3); } }, WAIT);
  } else if (mode === 'query-ephemeral') {
    /* source port is ephemeral: the A reply lands on a port no system mDNS
     * service can steal, isolating wire correctness from Windows reuse races */
    const s2 = dgram.createSocket({ type: 'udp4' });
    const packet = queryPacket(name);
    let done = false;
    s2.on('message', (msg, rinfo) => {
      const a = parseAnswer(msg);
      if (a && a.name === name.toLowerCase()) {
        console.log(`MDNS_PEER_ANSWER name=${name} ip=${a.ip} ttl=${a.ttl} from=${rinfo.address}:${rinfo.port}`);
        done = true; s2.close(); process.exit(0);
      }
    });
    s2.bind(0, LAN || '0.0.0.0', () => {
      const target = DIRECT || Group;
      s2.send(packet, 0, packet.length, Port, target, (e) => { if (e) { console.log('MDNS_PEER_ERROR ' + e.code); process.exit(2); } });
      setTimeout(() => { if (!done) { console.log('MDNS_PEER_TIMEOUT name=' + name + ' ephemeral'); s2.close(); process.exit(3); } }, WAIT);
    });
  } else if (mode === 'observe') {
    /* listen-only on 5353+group: log announcements/queries for diagnostics */
    console.log('MDNS_PEER_LISTEN observe');
    s.on('message', (msg, rinfo) => {
      const a = parseAnswer(msg);
      if (a) console.log(`MDNS_PEER_ANSWER_OBSERVED name=${a.name} ip=${a.ip} ttl=${a.ttl} from=${rinfo.address}`);
      else if (!isAnswer(msg)) console.log(`MDNS_PEER_QUERY_OBSERVED name=${questionName(msg)} qtype=${questionType(msg)} from=${rinfo.address}`);
    });
    setTimeout(() => { s.close(); process.exit(0); }, SECONDS * 1000);
  } else if (mode === 'answer') {
    const ip = extra; if (!ip) throw Error('answer needs ip');
    console.log('MDNS_PEER_LISTEN answer ' + name + ' ' + ip);
    const t0 = Date.now();
    s.on('message', (msg, rinfo) => {
      const observed = questionName(msg);
      if (observed && !isAnswer(msg)) console.log(`MDNS_PEER_QUERY_OBSERVED name=${observed} from=${rinfo.address}`);
      if (isAnswer(msg)) { const a = parseAnswer(msg); if (a) console.log(`MDNS_PEER_ANSWER_OBSERVED name=${a.name} ip=${a.ip} from=${rinfo.address}`); return; }
      const tail = questionTail(msg);
      if (observed === name.toLowerCase() && (questionType(msg) === 1 || questionType(msg) === 0xff) && tail) {
        const ans = Buffer.concat([
          Buffer.from([msg[0], msg[1], 0x84, 0, 0, 1, 0, 1, 0, 0, 0, 0]), tail,
          Buffer.from([0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 120, 0, 4]),
          Buffer.from(ip.split('.').map(Number))]);
        s.send(ans, 0, ans.length, rinfo.port, rinfo.address);
        console.log(`MDNS_PEER_ANSWERED name=${observed} ip=${ip} to=${rinfo.address}`);
      }
    });
    setInterval(() => { if (Date.now() - t0 > SECONDS * 1000) { s.close(); process.exit(0); } }, 1000).unref();
    process.on('SIGTERM', () => process.exit(0));
  } else if (mode === 'conflict') {
    const ip = extra; if (!ip) throw Error('conflict needs ip');
    let n = 0;
    const target = DIRECT || Group;
    const fire = () => {
      const q = encode(name);
      const pkt = Buffer.concat([
        Buffer.from([0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0]), q,
        Buffer.from([0, 1, 0, 1, 0, 0, 0, 120, 0, 4]),
        Buffer.from(ip.split('.').map(Number))]);
      s.send(pkt, 0, pkt.length, Port, target, () => {
        console.log(`MDNS_PEER_CONFLICT sent name=${name} ip=${ip} target=${target} n=${++n}`);
        if (n >= SECONDS) { s.close(); process.exit(0); }
      });
    };
    fire();
    setInterval(fire, 1000).unref();
    process.on('SIGTERM', () => process.exit(0));
  } else throw Error('mode?');
}
openSocket();
