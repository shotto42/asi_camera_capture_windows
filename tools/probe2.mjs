// probe2.mjs — follow redirects, print final URL + status + size
import https from 'node:https';
import http from 'node:http';
const urls = process.argv.slice(2);
function go(u, n = 8) {
  return new Promise((resolve) => {
    const lib = u.startsWith('https') ? https : http;
    lib.get(u, { headers: { 'User-Agent': 'Mozilla/5.0' } }, (r) => {
      if ([301, 302, 303, 307, 308].includes(r.statusCode) && r.headers.location && n > 0) {
        r.resume();
        return resolve(go(new URL(r.headers.location, u).href, n - 1));
      }
      const len = r.headers['content-length'];
      console.log(`${r.statusCode}  ${len ? (parseInt(len) / 1048576).toFixed(1) + ' MB' : '-'}  final=${u}`);
      r.resume(); r.on('end', () => resolve(0));
    }).on('error', (e) => { console.log('ERR ' + e.message + '  ' + u); resolve(0); });
  });
}
for (const u of urls) { await go(u); }
