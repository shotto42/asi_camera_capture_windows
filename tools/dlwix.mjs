// Find + download the WiX Toolset v3.14 x64 installer (candle/light/heat)
// from the wix3 GitHub releases. Node is the only working HTTPS client on
// this box (PowerShell/curl TLS is broken: SEC_E_NO_CREDENTIALS).
import fs from "node:fs";

const api = await fetch("https://api.github.com/repos/wixtoolset/wix3/releases", {
  headers: { "User-Agent": "asi-camera-app" },
});
if (!api.ok) {
  console.log("releases API status:", api.status);
  process.exit(1);
}
const rels = await api.json();
for (const rel of rels.slice(0, 5)) {
  const names = rel.assets.map((a) => a.name).join(", ");
  console.log(`tag=${rel.tag_name} assets=${names}`);
}
// prefer the latest v3.14.x, take the x64 msi
const rel = rels.find((r) => r.tag_name === "wix3141rtm") ?? rels[0];
const asset = rel.assets.find((a) => a.name === "wix314-binaries.zip");
if (!asset) {
  console.log("no wix314-binaries.zip asset found");
  process.exit(1);
}
console.log("downloading", asset.browser_download_url);
const r = await fetch(asset.browser_download_url, { redirect: "follow" });
if (!r.ok) {
  console.log("download status:", r.status);
  process.exit(1);
}
const buf = Buffer.from(await r.arrayBuffer());
const out = "third_party/dl/wix314-binaries.zip";
fs.writeFileSync(out, buf);
console.log("wrote", out, buf.length, "bytes");
