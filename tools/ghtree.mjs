// ghtree.mjs — full recursive tree of gstreamer/gstreamer at a tag, grep a needle
import https from 'node:https';
const ref = process.argv[2] || '1.24.12';
const needle = process.argv[3] || 'isomp4';
// get tag sha
const sha = await new Promise((res) => {
  https.get(`https://api.github.com/repos/gstreamer/gstreamer/git/ref/tags/${ref}`, { headers: { 'User-Agent': 'm', Accept: 'application/vnd.github+json' } }, (r) => {
    let d = '';
    r.on('data', (c) => (d += c));
    r.on('end', () => {
      try { const j = JSON.parse(d); res(j.object?.type === 'tag' ? j.object.sha : (j.object?.sha || null)); } catch { res(null); }
    });
  }).on('error', () => res(null));
});
console.log('tag object sha:', sha);
// for annotated tags, object.type is 'tag' -> need peeled commit; GitHub trees API accepts tag name directly though
let tree = null;
tree = await new Promise((res) => {
  https.get(`https://api.github.com/repos/gstreamer/gstreamer/git/trees/${ref}?recursive=1`, { headers: { 'User-Agent': 'm', Accept: 'application/vnd.github+json' } }, (r) => {
    let d = '';
    r.on('data', (c) => (d += c));
    r.on('end', () => {
      try { res(JSON.parse(d)); } catch { res(null); }
    });
  }).on('error', () => res(null));
});
if (!tree || !tree.tree) { console.log('tree fetch failed'); process.exit(1); }
console.log(`total entries: ${tree.tree.length} truncated=${tree.truncated}`);
const hits = tree.tree.filter((t) => t.path.includes(needle));
console.log(`${hits.length} entries matching "${needle}":`);
hits.slice(0, 60).forEach((h) => console.log(`  ${h.type}  ${h.path}`));
