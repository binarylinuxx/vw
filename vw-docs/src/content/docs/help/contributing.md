---
title: Contributing
description: How the code is laid out, how to build and check it, and how these docs are made.
---

## Layout

| Path | Contents |
| --- | --- |
| `include/vw/` | the public C++ API; `vw.hpp` includes all of it |
| `src/` | the library: Wayland frontend, renderer, text and images, services |
| `src/lua/` | the deprecated Lua frontend |
| `shaders/` | the renderer's compute shaders, embedded as `.vws` |
| `tools/` | `vw-shaderc` |
| `examples/cpp/` | `bar`, `launcher`, `status`, `tasks` |
| `vw-docs/` | this site |

## Build and check

```sh
nix develop
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build        # frontend smoke tests
```

The project builds with clang; keep it warning-free under `-Wall -Wextra -Wpedantic`.

## These docs

The site is Astro with MDX, in `vw-docs/`:

```sh
cd vw-docs
bun install
bun run dev        # http://localhost:4321, live reload
bun run build      # static site in dist/
```

Pages are Markdown or MDX under `src/content/docs/`; the sidebar order lives in `src/data/navigation.ts`. MDX pages can use `Callout`, `Tabs` and `Api` from `src/components`. Keep pages true to the headers: when an API changes, change its page in the same commit.
