import { defineConfig } from 'astro/config';
import mdx from '@astrojs/mdx';
import facet from './src/data/facet.tmLanguage.json' with { type: 'json' };

export default defineConfig({
  site: process.env.DOCS_SITE || 'https://vw.zapzig.org',
  base: process.env.DOCS_BASE || '/',
  integrations: [mdx()],
  markdown: {
    shikiConfig: {
      themes: { light: 'github-light', dark: 'github-dark-dimmed' },
      defaultColor: false,
      // Facet's own grammar; cpp { } blocks inside it highlight as C++.
      langs: ['cpp', { ...facet, embeddedLangs: ['cpp'] }],
    },
  },
  // The toolbar's client scripts stall some phone browsers.
  devToolbar: { enabled: false },
  server: { host: true, port: 4321, allowedHosts: ['.lhr.life', 'vw.zapzig.org'] },
});
