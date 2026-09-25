// chkpkg.mjs — download a MSYS2 pkg and list files matching a needle
import { createWriteStream, mkdirSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import https from 'node:https';
import path from 'node:path';

const base = 'https://repo.msys2.org/mingw/mingw64/';
const [file, needle, outdir] = process.argv.slice(2);

async function dl(url, out) {
  await new Promise((res, rej) => {
    https.get(url, { headers: { 'User-Agent': 'Mozilla/5.0' } }, (r) => {
      if (r.statusCode !== 200) { rej(new Error('HTTP ' + r.statusCode)); r.resume(); return; }
      const f = createWriteStream(out);
      r.pipe(f);
      f.on('finish', () => { f.close(); res(); });
    }).on('error', rej);
  });
}
mkdirSync(outdir, { recursive: true });
const out = path.join(outdir, file);
await dl(base + file, out);
const dest = path.join(outdir, 'chk-' + file.replace(/[^a-z0-9]/gi, '-'));
mkdirSync(dest, { recursive: true });
execFileSync('tar', ['-xf', out, '-C', dest]);
const list = execFileSync('tar', ['-tf', out], { encoding: 'utf8' });
const lines = list.split('\n').filter((l) => l && (needle ? l.includes(needle) : true));
console.log(`${file}: ${lines.length} matching lines`);
lines.slice(0, 60).forEach((l) => console.log('  ' + l));
