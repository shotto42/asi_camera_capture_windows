// dl.mjs — download helper with redirect following (system schannel TLS is broken; Node TLS works)
// usage: node tools/dl.mjs <url> <outfile>
import { createWriteStream, mkdirSync } from 'node:fs';
import { dirname } from 'node:path';
import https from 'node:https';
import http from 'node:http';

const [url0, out] = process.argv.slice(2);
if (!url0 || !out) { console.error('usage: node tools/dl.mjs <url> <outfile>'); process.exit(2); }
mkdirSync(dirname(out) || '.', { recursive: true });

let total = 0, got = 0, lastLog = 0;
function log() {
  const now = Date.now();
  if (now - lastLog < 3000 && total) return;
  lastLog = now;
  if (total) console.log(`${(got / total * 100).toFixed(1)}%  ${(got / 1048576).toFixed(0)} MB / ${(total / 1048576).toFixed(0)} MB`);
}
function go(u, redirectsLeft) {
  const lib = u.startsWith('https') ? https : http;
  lib.get(u, { headers: { 'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)' } }, (res) => {
    if ([301, 302, 307, 308].includes(res.statusCode) && res.headers.location) {
      res.resume();
      if (redirectsLeft-- > 0) return go(new URL(res.headers.location, u).href, redirectsLeft);
      console.error('too many redirects'); process.exit(1);
    }
    if (res.statusCode !== 200) {
      res.resume();
      console.error(`HTTP ${res.statusCode} for ${u}`);
      process.exit(1);
    }
    total = parseInt(res.headers['content-length'] || '0', 10);
    console.log(`GET ${u}\n  (${total ? (total / 1048576).toFixed(1) + ' MB' : 'unknown size'}) -> ${out}`);
    const f = createWriteStream(out);
    res.on('data', (c) => { got += c.length; log(); });
    res.pipe(f);
    f.on('finish', () => { f.close(); console.log(`OK ${out} (${got} bytes)`); process.exit(0); });
    f.on('error', (e) => { console.error('write error', e.message); process.exit(1); });
  }).on('error', (e) => { console.error('fetch failed:', e.message); process.exit(1); });
}
go(url0, 10);
