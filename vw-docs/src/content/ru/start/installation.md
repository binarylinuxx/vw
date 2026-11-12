---
title: Установка и сборка
description: Всё собирается CMake и clang внутри dev shell проекта на Nix; библиотека ставит CMake-пакет, который находят другие программы.
---

## С Nix

Флейк даёт dev shell с clang и всеми зависимостями, а также пакет.

```sh
nix develop                     # clang, cmake, ninja, glslc, все библиотеки
cmake -S . -B build -G Ninja
cmake --build build
```

`nix build` собирает `libvw`, её заголовки, шейдеры и Lua-фронтенд `vw`; `nix run` запускает фронтенд.

## Новый проект

Команда `vw` создаёт новый проект, названный по имени папки, и собирает его:

```sh
vw --init my-widget             # CMakeLists.txt, main.cpp, .gitignore; настройка cmake + сборка
vw --init-nix my-widget         # то же плюс flake.nix: git add, nix flake lock, сборка в его dev shell
./my-widget/build/my-widget
```

`--init` собирает clang и Ninja, если они установлены, и указывает CMake на тот vw, с которым пришла команда. Ни одна из команд не перезаписывает существующие файлы.

## В проекте на Nix flake

Без установленного `vw` начните с шаблона флейка — в нём те же файлы:

```sh
mkdir my-widget && cd my-widget
nix flake init -t github:binarylinuxx/vw
git init && git add -A          # флейки видят только файлы под git
nix run                         # собрать и запустить
nix develop                     # или работать с cmake руками
```

Чтобы упаковать оболочку из уже существующего флейка, передайте её в `vw.lib.mkShell`. Он собирает ваш `CMakeLists.txt` с clang, Ninja и glslc, линкует vw статически, чтобы пакет был только вашей оболочкой, и выбрасывает из исходников локальную `build/`:

```nix
{
  inputs.vw.url = "github:binarylinuxx/vw";

  outputs = { self, vw }:
    let
      system = "x86_64-linux";
      pkgs = vw.inputs.nixpkgs.legacyPackages.${system};
    in {
      packages.${system}.default = vw.lib.mkShell {
        inherit system;
        pname = "my-shell";
        src = ./.;
        shellDeps = [ pkgs.libnotify ];        # библиотеки, в дополнение к библиотекам vw
        nativeShellDeps = [ pkgs.python3 ];    # инструменты сборки, в дополнение к cmake, ninja, glslc
      };
      devShells.${system}.default = self.packages.${system}.default.devShell;
    };
}
```

Все остальные атрибуты (`version`, `cmakeFlags`, `postInstall`, `meta`, …) уходят прямо в `mkDerivation`. `.devShell` — это shell с clang и всем, с чем собирается пакет, для работы с `cmake` вручную.

`facet = true;` собирает с vw, в котором есть [Facet](/ru/facet/introduction/) — декларативный язык описания интерфейса: `facet_compile()` приходит вместе с `find_package(vw)`. Без этой опции Facet никак не участвует.

Для полного контроля возьмите overlay: он добавляет `pkgs.vw` и `pkgs.vw-static` к остальному nixpkgs, а `find_package(vw)` работает, как описано ниже.

```nix
pkgs = import nixpkgs { inherit system; overlays = [ vw.overlays.default ]; };
# затем: buildInputs = [ pkgs.vw-static ]; nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
```

| Выход флейка | Что это |
| --- | --- |
| `overlays.default` | добавляет `pkgs.vw` и `pkgs.vw-static` |
| `packages.<system>.default`, `.vw` | библиотека, заголовки, CMake-пакет, `vw-shaderc`, примеры |
| `packages.<system>.vw-static` | то же с `libvw.a`; в overlay — `pkgs.vw-static` |
| `packages.<system>.vw-facet` | vw с Facet; в overlay — `pkgs.vw-facet`, `pkgs.vw-static-facet` |
| `devShells.<system>.default` | всё, что нужно для работы над самим vw |
| `lib.mkShell` | оболочка на CMake как пакет, vw влинкован; `.devShell` — для работы над ней |
| `templates.default` | проект на CMake, использующий vw |

## Без Nix

Нужен компилятор C++23 (vw разрабатывается на clang), CMake, Ninja, `pkg-config`, `glslc` (shaderc) и `wayland-scanner`, а также:

| Библиотека | Для чего |
| --- | --- |
| загрузчик и заголовки Vulkan | отрисовка |
| freetype, fontconfig, harfbuzz, pango(cairo) | текст |
| librsvg, libwebp, cairo | картинки SVG, WebP и canvas |
| gio, gtk3 | сервисы D-Bus, иконки темы |
| pipewire | звук |
| libxkbcommon | клавиатуры |
| pam | экраны блокировки |
| quickjs | canvas на JavaScript |
| nlohmann_json | IPC композиторов |
| Lua 5.4 | только с `VW_WITH_LUA` |

## Параметры сборки

| Параметр | По умолчанию | Что делает |
| --- | --- | --- |
| `VW_BUILD_EXAMPLES` | `ON` | собирает программы `vw-example-*` из `examples/cpp` |
| `VW_WITH_FACET` | `OFF` | собирает [Facet](/ru/facet/setup/): `vw-facetc`, `<vw/facet.hpp>` и `facet_compile()` |
| `VW_STATIC` | `OFF` | собирает `libvw.a`, которая влинковывается в вашу программу: оболочка поставляется одним бинарником, и ставить vw отдельно не нужно |
| `VW_WITH_LUA` | `ON` | собирает устаревший Lua-фронтенд `vw` |
| `BUILD_TESTING` | как в CTest | добавляет smoke-тесты фронтенда |

## Подключение к своему проекту

Установите vw или укажите проекту на исходники:

```cmake
find_package(vw 0.1 REQUIRED)          # установленная libvw
# или: add_subdirectory(path/to/vw vw EXCLUDE_FROM_ALL)
target_link_libraries(my-shell PRIVATE vw::vw)
```

В вашу сборку протекает только `nlohmann_json`; Wayland, Vulkan и библиотеки тулкита приватны. Собственные шейдеры vw встроены в библиотеку, так что во время работы ничего не нужно ставить или искать. Пакет также приносит `vw-shaderc` и `vw_compile_shader()` для [ваших собственных шейдеров](/ru/guides/shaders/).
