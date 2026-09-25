// rawlist.mjs — dump raw hrefs of a directory page (follows redirects), grep a substring
import https from 'node:https';
import http from 'node:http';
const [url0, needle] = process.argv.slice(2);
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
      console.log(`final: ${u} (${d.length} bytes)`);
      const names = [...d.matchAll(/href="([^"]+)"/g)].map((m) => m[1]);
      console.log(`total hrefs: ${names.length}`);
      if (needle) {
        const sel = names.filter((x) => x.includes(needle));
        console.log(`matching "${needle}": ${sel.length}`);
        sel.slice(0, 100).forEach((x) => console.log('  ' + x));
      } else {
        names.slice(0, 60).forEach((x) => console.log('  ' + x));
      }
    });
  }).on('error', (e) => { console.error('ERR', e.message); process.exit(1); });
}
go(url0);
