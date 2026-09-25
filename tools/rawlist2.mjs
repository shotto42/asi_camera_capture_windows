// rawlist2.mjs — list a directory (follow redirects), grep substring, print full href
import https from 'node:https';
import http from 'node:http';
const [url0, needle] = process.argv.slice(2);
let n = 0;
function go(u) {
  const lib = u.startsWith('https') ? https : http;
  lib.get(u, { headers: { 'User-Agent': 'Mozilla/5.0' } }, (res) => {
    if ([301, 302, 303, 307, 308].includes(res.statusCode) && res.headers.location && n++ < 10) {
      res.resume();
      return go(new URL(res.headers.location, u).href);
    }
    let d = '';
    res.on('data', (c) => (d += c));
    res.on('end', () => {
      console.log(`final: ${u} (${d.length} bytes) status ${res.statusCode}`);
      const names = [...d.matchAll(/href="([^"]+)"/g)].map((m) => m[1]);
      const sel = names.filter((x) => needle ? x.includes(needle) : true);
      console.log(`matched ${sel.length}:`);
      sel.slice(0, 80).forEach((x) => console.log('  ' + x));
    });
  }).on('error', (e) => { console.error('ERR', e.message); process.exit(1); });
}
go(url0);
