// probe3.mjs — probe candidate URLs (status + size)
import https from 'node:https';
import http from 'node:http';
const urls = process.argv.slice(2);
function probe(u) {
  return new Promise((res) => {
    const lib = u.startsWith('https') ? https : http;
    const req = lib.request(u, { method: 'HEAD', headers: { 'User-Agent': 'Mozilla/5.0' } }, (r) => {
      const len = r.headers['content-length'];
      console.log(`${r.statusCode}  ${len ? (parseInt(len) / 1048576).toFixed(1) + ' MB' : '-'}  ${u}`);
      r.resume(); r.on('end', () => res(0));
    });
    req.on('error', (e) => { console.log('ERR ' + e.message + '  ' + u); res(0); });
    req.end();
  });
}
for (const u of urls) { await probe(u); }
