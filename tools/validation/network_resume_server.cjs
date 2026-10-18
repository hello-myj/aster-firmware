/* SPDX-License-Identifier: MIT. Controlled Range/If-Range peer for N5.4
 * resume verification. Windows/Linux native Node, builtins only.
 * Usage: node network_resume_server.cjs
 * Routes (127.0.0.1:9082):
 *  /resume?n=SIZE&cut=K   pattern i%251, ETag "v1", Last-Modified fixed.
 *      Honors Range only with matching If-Range; cut=K aborts after ~K bytes
 *      (transport drop to force a real resume). First request is answered
 *      with a fresh validator each time (stable ETag for the same n).
 *  /novalue               ignores Range entirely: always 200 full body with
 *      an ETag that CHANGES every response (content version flips): a correct
 *      client must never stitch, always restart.
 *  /gone                  ETag flips while Range honored: 206 on mismatch is
 *      disallowed; must answer 200.
 * Logs one JSON line per request for the harness to assert.
 */
'use strict';
const http = require('http');
const pattern = (n) => { const b = Buffer.alloc(n); for (let i = 0; i < n; i++) b[i] = i % 251; return b; };
let seen = 0;
const handler = (req, res) => {
  const url = new URL(req.url, 'http://fixture.invalid');
  const n = Math.min(4194304, Math.max(1, Number(url.searchParams.get('n') || 65536)));
  const cut = Math.max(0, Number(url.searchParams.get('cut') || 0));
  const body = pattern(n);
  const range = req.headers.range, ifRange = req.headers['if-range'];
  ++seen;
  const record = { req: seen, path: url.pathname, range: range || null, ifRange: ifRange || null };
  const send = (status, slice, start, total, headers) => {
    const declared = status === 206 ? 'bytes ' + start + '-' + (total - 1) + '/' + total : null;
    res.writeHead(status, { 'Content-Length': slice.length, 'Connection': 'close',
      ...(declared ? { 'Content-Range': declared } : {}), ...headers });
    record.status = status; record.sent = slice.length;
    /* Truncate only the initial full-body attempt: one mid-stream drop, then
     * the 206 continuation must complete so the file can be committed. */
    const drop = cut > 0 && start === 0 && slice.length > cut;
    if (req.method === 'HEAD' || !drop) { if (req.method !== 'HEAD') res.write(slice); res.end(); console.log(JSON.stringify(record)); return; }
    res.write(slice.subarray(0, cut));
    record.droppedAt = cut;
    setTimeout(() => res.destroy(), 20);
    console.log(JSON.stringify(record));
  };
  if (url.pathname === '/novalue') {
    const etag = '"flip-' + seen + '"'; /* validator always changes */
    return send(200, body, 0, n, { 'ETag': etag, 'Accept-Ranges': 'bytes' });
  }
  const etag = '"resume-v1-' + n + '"', lastmod = 'Wed, 21 Oct 2026 00:00:00 GMT';
  if (url.pathname === '/gone') {
    /* always a fresh validator: an old If-Range never matches */
    const fresh = '"flip-' + seen + '"';
    if (range && ifRange === etag) return send(206, body.subarray(Number(range.split('=')[1].split('-')[0])), Number(range.split('=')[1].split('-')[0]), n, { 'Content-Range': 'bytes ' + range.split('=')[1].split('-')[0] + '-' + (n - 1) + '/' + n, 'ETag': fresh });
    return send(200, body, 0, n, { 'ETag': fresh });
  }
  if (url.pathname === '/resume') {
    let m = range && /^bytes=(\d+)\-(\d*)$/.exec(range);
    if (m && ifRange && ifRange !== etag && ifRange !== lastmod) {
      return send(200, body, 0, n, { 'ETag': etag, 'Last-Modified': lastmod }); /* If-Range mismatch */
    }
    if (m && ifRange && Number(m[1]) >= n) {
      res.writeHead(416, { 'Content-Range': 'bytes */' + n, 'Content-Length': 0, 'Connection': 'close' });
      console.log(JSON.stringify({ ...record, status: 416 })); return res.end();
    }
    if (m && ifRange) {
      const start = Number(m[1]);
      return send(206, body.subarray(start), start, n, { 'Content-Range': `bytes ${start}-${n - 1}/${n}`, 'ETag': etag, 'Last-Modified': lastmod });
    }
    return send(200, body, 0, n, { 'ETag': etag, 'Last-Modified': lastmod });
  }
  res.writeHead(404); res.end(); console.log(JSON.stringify({ ...record, status: 404 }));
};
http.createServer(handler).listen(9082, '127.0.0.1', () => console.log(JSON.stringify({ ready: true, http: 9082 })));
process.on('SIGTERM', () => process.exit(0));
