// ghlist.mjs — list git refs (tags/branches) of gstreamer/gstreamer matching a filter
import https from 'node:https';
const filter = process.argv[2] || '1.24';
let page = 1;
while (page <= 5) {
  const ok = await new Promise((res) => {
    https.get(`https://api.github.com/repos/gstreamer/gstreamer/git/matching-refs/tags/${filter}?per_page=100&page=${page}`, { headers: { 'User-Agent': 'm', Accept: 'application/vnd.github+json' } }, (r) => {
      let d = '';
      r.on('data', (c) => (d += c));
      r.on('end', () => {
        if (r.statusCode !== 200) { console.log(`status ${r.statusCode}`); console.log(d.slice(0, 200)); return res(false); }
        const arr = JSON.parse(d);
        arr.forEach((t) => console.log(`  ${t.ref.replace('refs/tags/', '')}  ->  ${t.object.sha.slice(0, 12)}`));
        return res(arr.length === 100);
      });
    }).on('error', (e) => { console.log('ERR', e.message); res(false); });
  });
  if (!ok) break;
  page++;
}
