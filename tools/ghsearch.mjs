// ghsearch.mjs — GitHub code search for a filename in gstreamer/gstreamer at a tag
import https from 'node:https';
const q = encodeURIComponent('mp4mux.c in:file repo:gstreamer/gstreamer');
await new Promise((res) => {
  https.get(`https://api.github.com/search/code?q=${q}&per_page=20`, { headers: { 'User-Agent': 'm', Accept: 'application/vnd.github+json' } }, (r) => {
    let d = '';
    r.on('data', (c) => (d += c));
    r.on('end', () => {
      console.log(`status ${r.statusCode}`);
      if (r.statusCode !== 200) { console.log(d.slice(0, 400)); return res(0); }
      const j = JSON.parse(d);
      console.log(`total_count: ${j.total_count}`);
      (j.items || []).forEach((it) => console.log(`  ${it.path}  (${it.repository?.full_name})`));
      res(0);
    });
  }).on('error', (e) => { console.log('ERR', e.message); res(0); });
});
