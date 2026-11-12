# vw documentation

Native Astro + MDX with a custom wiki interface, managed with Bun.

```sh
bun install
bun run dev
```

Open the local URL printed by Astro. Production commands:

```sh
bun run check
bun run build
bun run check:links
bun run preview
```

Pages live in `src/content/docs/`; navigation is in `src/data/navigation.ts`.
Layout is in `src/layouts/DocsLayout.astro`, with custom styles in
`src/styles/docs.css`. Search lazily loads the generated `search-index.json`
and supports full-text queries, Ctrl/Cmd+K, arrow navigation, and Enter to open.
The site is static and does not depend on a documentation theme engine.

Screenshots intentionally start as labeled placeholders. Drop images into
`public/screenshots/` using the names in that directory's README, then rebuild.

Production is https://vw.zapzig.org, deployed by `.github/workflows/docs.yml`. Override with `DOCS_SITE` and
`DOCS_BASE=/vw/` if deploying under a subpath.
Deploy the generated `dist/` directory to any static host.

Theme preference is saved locally in the browser.
