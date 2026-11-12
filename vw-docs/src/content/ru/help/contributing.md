---
title: Участие в разработке
description: Как устроен код, как его собрать и проверить и как сделана эта документация.
---

## Структура

| Путь | Содержимое |
| --- | --- |
| `include/vw/` | публичный API на C++; `vw.hpp` подключает его целиком |
| `src/` | библиотека: фронтенд Wayland, рендерер, текст и картинки, сервисы |
| `src/lua/` | устаревший Lua-фронтенд |
| `shaders/` | compute-шейдеры рендерера, встроенные как `.vws` |
| `tools/` | `vw-shaderc` |
| `examples/cpp/` | `bar`, `launcher`, `status`, `tasks` |
| `vw-docs/` | этот сайт |

## Сборка и проверка

```sh
nix develop
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build        # smoke-тесты фронтенда
```

Проект собирается clang; держите его без предупреждений под `-Wall -Wextra -Wpedantic`.

## Эта документация

Сайт — Astro с MDX, в `vw-docs/`:

```sh
cd vw-docs
bun install
bun run dev        # http://localhost:4321, живая перезагрузка
bun run build      # статический сайт в dist/
```

Страницы — Markdown или MDX в `src/content/docs/`, русские переводы — под теми же путями в `src/content/ru/`; порядок в боковой панели задан в `src/data/navigation.ts`, а подписи интерфейса и меню на русском — в `src/data/i18n.ts`. Страницы MDX могут использовать `Callout`, `Tabs` и `Api` из `src/components`. Держите страницы в соответствии с заголовками: когда меняется API, меняйте его страницу в том же коммите. Страница без перевода показывается под `/ru/` на английском с пометкой.
