// gettext.mjs — fetch a URL as text
import https from 'node:https';
const u = process.argv[2];
await new Promise((res) => {
  https.get(u, { headers: { 'User-Agent': 'Mozilla/5.0' } }, (r) => {
    let d = '';
    r.on('data', (c) => (d += c));
    r.on('end', () => { console.log(`status ${r.statusCode} ${d.length}b`); console.log(d.slice(0, 4000)); res(0); });
  }).on('error', (e) => { console.log('ERR', e.message); res(0); });
});
