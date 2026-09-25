// list specific candidate URLs with status + size
import https from 'node:https';
const urls = process.argv.slice(2);
for (const u of urls) {
  await new Promise((res) => {
    https.get(u, { headers: { 'User-Agent': 'Mozilla/5.0' } }, (r) => {
      const len = r.headers['content-length'];
      console.log(`${r.statusCode}  ${len ? (parseInt(len) / 1048576).toFixed(0) + ' MB' : '?'}  ${u}`);
      r.resume(); r.on('end', res);
    }).on('error', (e) => { console.log('ERR ' + e.message + ' ' + u); res(); });
  });
}
