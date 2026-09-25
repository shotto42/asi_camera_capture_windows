// gstsources.mjs — fetch isomp4 plugin sources from GStreamer 1.24.12
import { createWriteStream, mkdirSync } from 'node:fs';
import https from 'node:https';

const tag = '1.24.12';
const base = `https://gitlab.freedesktop.org/gstreamer/gstreamer/-/raw/${tag}/subprojects/gst-plugins-base/ext/isomp4/`;
const files = ['isomp4.c', 'isomp4.h', 'mp4mux.c', 'mp4mux.h', 'mp4parse.c', 'mp4parse.h',
               'mp4fragment.c', 'mp4fragment.h', 'meson.build', 'rtpmp4depay.c', 'rtpmp4depay.h'];
const out = 'third_party/gst-isomp4';
mkdirSync(out, { recursive: true });
for (const f of files) {
  await new Promise((res) => {
    https.get(base + f, { headers: { 'User-Agent': 'Mozilla/5.0' } }, (r) => {
      if (r.statusCode !== 200) { console.log(`-- ${f}: HTTP ${r.statusCode}`); r.resume(); res(); return; }
      const w = createWriteStream(`${out}/${f}`);
      r.pipe(w);
      w.on('finish', () => { w.close(); console.log(`OK ${f}`); res(); });
    }).on('error', (e) => { console.log(`-- ${f}: ERR ${e.message}`); res(); });
  });
}
console.log('done');
