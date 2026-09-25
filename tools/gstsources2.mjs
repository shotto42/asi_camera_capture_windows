// gstsources2.mjs — fetch isomp4 sources from the GitHub gstreamer mirror at tag 1.24.12
import { createWriteStream, mkdirSync } from 'node:fs';
import https from 'node:https';

const base = 'https://raw.githubusercontent.com/gstreamer/gstreamer/1.24.12/subprojects/gst-plugins-base/ext/isomp4/';
const files = ['isomp4.c', 'isomp4.h', 'mp4mux.c', 'mp4mux.h', 'mp4parse.c', 'mp4parse.h',
               'mp4fragment.c', 'mp4fragment.h', 'meson.build', 'rtpmp4depay.c', 'rtpmp4depay.h'];
const out = 'third_party/gst-isomp4';
mkdirSync(out, { recursive: true });
for (const f of files) {
  await new Promise((res) => {
    https.get(base + f, { headers: { 'User-Agent': 'Mozilla/5.0' } }, (r) => {
      if (r.statusCode !== 200) { console.log(`-- ${f}: HTTP ${r.statusCode}`); r.resume(); res(); return; }
      const w = createWriteStream(`${out}/${f}`);
      r.on('data', (c) => {
        if (c.slice(0, 100).includes('not found') || c.slice(0, 100).includes('<!doctype')) {
          // bot page or error — bail
          w.close(); res();
        }
      });
      r.pipe(w);
      w.on('finish', () => { w.close(); console.log(`OK ${f}`); res(); });
    }).on('error', (e) => { console.log(`-- ${f}: ERR ${e.message}`); res(); });
  });
}
console.log('done');
