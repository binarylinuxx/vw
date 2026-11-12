import { existsSync, statSync } from 'node:fs';
import { join, resolve } from 'node:path';

const dist = resolve('dist');
if (!existsSync(dist)) {
  console.error('Build the site before checking links: bun run build');
  process.exit(1);
}
const base = `/${(process.env.DOCS_BASE || '/').replace(/^\/+|\/+$/g, '')}/`.replace(/^\/\//, '/');
const origin = 'https://docs.invalid';
const failures = [];
const cache = new Map();
let checked = 0;

for await (const page of new Bun.Glob('**/*.html').scan(dist)) {
  const html = await Bun.file(join(dist, page)).text();
  const route = page === 'index.html' ? '' : page.replace(/index\.html$/, '');
  const current = new URL(`${base}${route}`, origin);
  for (const match of html.matchAll(/<a\b[^>]*\bhref="([^"]+)"[^>]*>/g)) {
    const target = new URL(match[1].replaceAll('&amp;', '&'), current);
    if (target.origin !== origin) continue;
    checked++;
    let pathname = target.pathname;
    if (pathname === base.replace(/\/$/, '')) pathname = base;
    if (!pathname.startsWith(base)) {
      failures.push(`${page}: link escapes the configured base: ${match[1]}`);
      continue;
    }
    let file = join(dist, decodeURIComponent(pathname.slice(base.length)));
    if (existsSync(file) && statSync(file).isDirectory()) file = join(file, 'index.html');
    if (!existsSync(file)) {
      failures.push(`${page}: missing target ${match[1]}`);
      continue;
    }
    if (target.hash && file.endsWith('.html')) {
      let ids = cache.get(file);
      if (!ids) {
        const contents = await Bun.file(file).text();
        ids = new Set([...contents.matchAll(/\bid="([^"]+)"/g)].map((item) => item[1]));
        cache.set(file, ids);
      }
      const fragment = decodeURIComponent(target.hash.slice(1));
      if (fragment && !ids.has(fragment)) failures.push(`${page}: missing anchor ${match[1]}`);
    }
  }
}
if (failures.length) {
  for (const failure of [...new Set(failures)]) console.error(failure);
  process.exit(1);
}
console.log(`Checked ${checked} internal links and anchors. No broken targets.`);
