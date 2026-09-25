// dlall.mjs — batch download of build dependencies
import { createWriteStream, mkdirSync } from 'node:fs';
import { dirname } from 'node:path';
import https from 'node:https';
import http from 'node:http';

const baseQt = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_673/qt.qt6.673.win64_msvc2019_64/';
const baseMs = 'https://repo.msys2.org/mingw/mingw64/';
const OUT = 'third_party/dl';

const files = [
  ['https://github.com/Kitware/CMake/releases/download/v3.29.6/cmake-3.29.6-windows-x86_64.zip', `${OUT}/cmake-3.29.6-windows-x86_64.zip`],
  ['https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip', `${OUT}/ninja-win.zip`],
  [baseQt + '6.7.3-0-202409200836qtbase-Windows-Windows_10_22H2-MSVC2019-Windows-Windows_10_22H2-X86_64.7z', `${OUT}/qtbase-6.7.3.7z`],
  [baseQt + '6.7.3-0-202409200836qttools-Windows-Windows_10_22H2-MSVC2019-Windows-Windows_10_22H2-X86_64.7z', `${OUT}/qttools-6.7.3.7z`],
  ['https://github.com/opencv/opencv/releases/download/4.12.0/opencv-4.12.0-windows.exe', `${OUT}/opencv-4.12.0-windows.exe`],
  [baseMs + 'mingw-w64-x86_64-gstreamer-1.24.12-1-any.pkg.tar.zst', `${OUT}/gstreamer-1.24.12.pkg.tar.zst`],
  [baseMs + 'mingw-w64-x86_64-gst-plugins-base-1.24.12-1-any.pkg.tar.zst', `${OUT}/gst-plugins-base-1.24.12.pkg.tar.zst`],
  [baseMs + 'mingw-w64-x86_64-gst-plugins-ugly-1.24.12-2-any.pkg.tar.zst', `${OUT}/gst-plugins-ugly-1.24.12.pkg.tar.zst`],
  [baseMs + 'mingw-w64-x86_64-glib2-2.84.3-2-any.pkg.tar.zst', `${OUT}/glib2-2.84.3.pkg.tar.zst`],
  [baseMs + 'mingw-w64-x86_64-libx264-0.165.r3222.b35605a-3-any.pkg.tar.zst', `${OUT}/libx264.pkg.tar.zst`],
  [baseMs + 'mingw-w64-x86_64-pcre2-10.48-3-any.pkg.tar.zst', `${OUT}/pcre2.pkg.tar.zst`],
  [baseMs + 'mingw-w64-x86_64-libffi-3.8.0-1-any.pkg.tar.zst', `${OUT}/libffi.pkg.tar.zst`],
  [baseMs + 'mingw-w64-x86_64-zlib-1.3.2-2-any.pkg.tar.zst', `${OUT}/zlib.pkg.tar.zst`],
  [baseMs + 'mingw-w64-x86_64-zstd-1.5.7-2-any.pkg.tar.zst', `${OUT}/zstd.pkg.tar.zst`],
  [baseMs + 'mingw-w64-x86_64-libwinpthread-14.0.0.r353.g6df76fa52-2-any.pkg.tar.zst', `${OUT}/libwinpthread.pkg.tar.zst`],
  [baseMs + 'mingw-w64-x86_64-gcc-libs-16.2.0-4-any.pkg.tar.zst', `${OUT}/gcc-libs.pkg.tar.zst`],
];

function fetchStream(u, onSettled, onResponse) {
  const lib = u.startsWith('https') ? https : http;
  lib.get(u, { headers: { 'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)' } }, onResponse)
    .on('error', onSettled);
}

function downloadOne(url, out) {
  return new Promise((resolve, reject) => {
    fetchStream(url, reject, (res) => {
      if ([301, 302, 307, 308].includes(res.statusCode) && res.headers.location) {
        res.resume();
        const u2 = new URL(res.headers.location, url).href;
        downloadOne(u2, out).then(resolve, reject);
        return;
      }
      if (res.statusCode !== 200) { res.resume(); reject(new Error(`HTTP ${res.statusCode}`)); return; }
      const total = parseInt(res.headers['content-length'] || '0', 10);
      let got = 0, last = 0;
      const f = createWriteStream(out);
      res.on('data', (c) => {
        got += c.length;
        const now = Date.now();
        if (total && now - last > 5000) { last = now; console.log(`  ${(got / total * 100).toFixed(1)}% ${(got / 1048576).toFixed(0)}/${(total / 1048576).toFixed(0)} MB`); }
      });
      res.on('error', reject);
      res.pipe(f);
      f.on('finish', () => { f.close(); console.log(`OK ${out} (${got} bytes)`); resolve(); });
      f.on('error', reject);
    });
  });
}

async function one(url, out) {
  mkdirSync(dirname(out), { recursive: true });
  for (let attempt = 1; attempt <= 3; attempt++) {
    try {
      await downloadOne(url, out);
      return;
    } catch (e) {
      console.log(`  retry ${attempt}: ${e.message}`);
      if (attempt === 3) throw e;
    }
  }
}

const t0 = Date.now();
for (const [u, o] of files) {
  console.log(`GET ${u}`);
  await one(u, o);
}
console.log(`ALL DONE in ${((Date.now() - t0) / 1000).toFixed(0)}s`);
