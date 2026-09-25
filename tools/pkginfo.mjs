// pkginfo.mjs — fetch MSYS2 pkginfo.json files and print dependencies
import https from 'node:https';
const base = 'https://repo.msys2.org/mingw/mingw64/';
const pkgs = process.argv.slice(2);
for (const p of pkgs) {
  await new Promise((res) => {
    https.get(base + p + '.pkginfo.json', { headers: { 'User-Agent': 'Mozilla/5.0' } }, (r) => {
      if (r.statusCode !== 200) { console.log(`-- ${p}: HTTP ${r.statusCode}`); r.resume(); r.on('end', res); return; }
      let d = '';
      r.on('data', (c) => (d += c));
      r.on('end', () => {
        try {
          const j = JSON.parse(d);
          console.log(`-- ${j.name} ${j.version}`);
          console.log(`   deps: ${(j.depends || []).join(', ')}`);
        } catch (e) { console.log(`-- ${p}: parse error`); }
        res();
      });
    }).on('error', (e) => { console.log(`-- ${p}: ERR ${e.message}`); res(); });
  });
}
