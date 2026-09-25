// ghcheck.mjs — check GitHub repo/mirror existence via API
import https from 'node:https';
const repos = process.argv.slice(2);
for (const r of repos) {
  await new Promise((res) => {
    https.get('https://api.github.com/repos/' + r, { headers: { 'User-Agent': 'm', Accept: 'application/vnd.github+json' } }, (resp) => {
      let d = '';
      resp.on('data', (c) => (d += c));
      resp.on('end', () => {
        if (resp.statusCode !== 200) { console.log(`${r}: HTTP ${resp.statusCode}`); return res(0); }
        try {
          const j = JSON.parse(d);
          console.log(`${r}: OK branch=${j.default_branch} pushed=${j.pushed_at} size=${j.size}KB fork=${j.fork}`);
        } catch { console.log(`${r}: parse err`); }
        res(0);
      });
    }).on('error', (e) => { console.log(`${r}: ERR ${e.message}`); res(0); });
  });
}
