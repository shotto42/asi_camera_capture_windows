// pkgtar.mjs — download a MSYS2 package and list its .dll entries (no child processes)
import { createWriteStream, mkdirSync } from 'node:fs';
import https from 'node:https';
import path from 'node:path';
import zlib from 'node:zlib';

const base = 'https://repo.msys2.org/mingw/mingw64/';
const [file, needle] = process.argv.slice(2);
const out = path.join('third_party', 'chk', file);
mkdirSync(path.dirname(out), { recursive: true });
await new Promise((res, rej) => {
  https.get(base + file, { headers: { 'User-Agent': 'Mozilla/5.0' } }, (r) => {
    if (r.statusCode !== 200) { rej(new Error('HTTP ' + r.statusCode)); r.resume(); return; }
    const f = createWriteStream(out);
    r.pipe(f);
    f.on('finish', () => { f.close(); res(); });
  }).on('error', rej);
});
console.log(`downloaded ${file}`);
