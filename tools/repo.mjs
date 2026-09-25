// repo.mjs — list an HTTP directory index (follows redirects)
import https from 'node:https';
import http from 'node:http';
const [url0, filter] = process.argv.slice(2);
const rx = filter ? new RegExp(filter, 'i') : null;
let n = 0;
function go(u) {
  const lib = u.startsWith('https') ? https : http;
  lib.get(u, { headers: { 'User-Agent': 'Mozilla/5.0' } }, (res) => {
    if ([301, 302, 307, 308].includes(res.statusCode) && res.headers.location) {
      res.resume();
      if (n++ < 10) return go(new URL(res.headers.location, u).href);
      return;
    }
    let d = '';
    res.on('data', (c) => (d += c));
    res.on('end', () => {
      console.log(`status ${res.statusCode}, ${d.length} bytes, final: ${u}`);
      const names = [...d.matchAll(/href="([^"]+)"/g)].map((m) => m[1]);
      const sel = names.filter((x) => rx ? rx.test(x) : true);
      console.log(`matched ${sel.length} names:`);
      sel.slice(0, 250).forEach((x) => console.log('  ' + x));
    });
  }).on('error', (e) => { console.error('ERR', e.message); process.exit(1); });
}
go(url0);
