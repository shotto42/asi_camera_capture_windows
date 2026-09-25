// ghassets.mjs — list release assets for ip7z/7zip
import https from 'node:https';
const ref = process.argv[2] || '26.03';
await new Promise((res) => {
  https.get(`https://api.github.com/repos/ip7z/7zip/releases/tags/${ref}`, { headers: { 'User-Agent': 'm', Accept: 'application/vnd.github+json' } }, (r) => {
    let d = '';
    r.on('data', (c) => (d += c));
    r.on('end', () => {
      if (r.statusCode !== 200) { console.log(`status ${r.statusCode} ${d.slice(0, 200)}`); return res(0); }
      const j = JSON.parse(d);
      (j.assets || []).forEach((a) => console.log(`  ${a.name}  ${(a.size / 1048576).toFixed(1)}MB  ${a.browser_download_url}`));
      res(0);
    });
  }).on('error', (e) => { console.log('ERR', e.message); res(0); });
});
