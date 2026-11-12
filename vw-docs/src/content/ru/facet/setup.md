---
title: Сборка и инструменты
description: Как включить Facet, компилировать файлы .facet из CMake или Nix, команда vw-facetc и подсветка в редакторах.
---

## Включение

Facet выключен, пока вы его не попросите:

```sh
cmake -S . -B build -G Ninja -DVW_WITH_FACET=ON
```

Это собирает `vw-facetc` (экспортируется как `vw::facetc`) и ставит заголовок среды выполнения Facet, `<vw/facet.hpp>`. Без этой опции ничего из Facet не собирается и не ставится, и vw не меняется.

## CMake

vw, собранный с Facet, даёт `facet_compile()` через `find_package(vw)`:

```cmake
find_package(vw REQUIRED)
facet_compile(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/views.facet.hpp" SOURCE views.facet)
add_executable(my-shell main.cpp "${CMAKE_CURRENT_BINARY_DIR}/views.facet.hpp")
target_include_directories(my-shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(my-shell PRIVATE vw::vw)
```

| Аргумент | Значение |
| --- | --- |
| `OUTPUT`, `SOURCE` | заголовок, который писать, и `.facet`, который читать |
| `NAMESPACE` | куда попадают view; по умолчанию `views` |
| `MODULE` | откуда берутся элементы, написанные без модуля (см. [C++ в Facet](/ru/facet/cpp/#модули-компонентов)) |

## Nix

`vw.lib.mkShell` принимает `facet = true;` — тогда сборка идёт с vw, в котором есть Facet (по-прежнему влинкованным в ваш бинарник):

```nix
packages.${system}.default = vw.lib.mkShell {
  inherit system;
  pname = "my-shell";
  src = ./.;
  facet = true;
};
```

`packages.<system>.vw-facet` — это vw с Facet сам по себе.

## vw-facetc

```sh
vw-facetc views.facet -o views.facet.hpp [--namespace NAME] [--module PATH]
```

Без `-o` заголовок уходит в стандартный вывод — удобно посмотреть, во что превращается view. Ошибки печатаются как `file:line:column: error: …` со строкой и указателем.

## Редакторы

`facet/editors/micro/facet.yaml` подсвечивает Facet в micro: скопируйте его в `~/.config/micro/syntax/`.
