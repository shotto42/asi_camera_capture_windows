// ghdir.mjs — list a directory in the gstreamer mirror at a ref
import https from 'node:https';
const ref = process.argv[2] || '1.24.12';
const dir = process.argv[3] || 'subprojects/gst-plugins-base/ext/isomp4';
await new Promise((res) => {
  https.get(`https://api.github.com/repos/gstreamer/gstreamer/contents/${dir}?ref=${ref}`, { headers: { 'User-Agent': 'm', Accept: 'application/vnd.github+json' } }, (r) => {
    let d = '';
    r.on('data', (c) => (d += c));
    r.on('end', () => {
      console.log(`status ${r.statusCode}`);
      if (r.statusCode !== 200) { console.log(d.slice(0, 300)); return res(0); }
      const arr = JSON.parse(d);
      arr.forEach((f) => console.log(`  ${f.name}  ${f.size}`));
      res(0);
    });
  }).on('error', (e) => { console.log('ERR', e.message); res(0); });
});
