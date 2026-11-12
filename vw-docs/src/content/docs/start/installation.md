---
title: Install and build
description: Everything builds with CMake and clang inside the project's Nix dev shell; the library installs a CMake package other programs can find.
---

## With Nix

The flake provides a clang dev shell with every dependency, and a package.

```sh
nix develop                     # clang, cmake, ninja, glslc, every library
cmake -S . -B build -G Ninja
cmake --build build
```

`nix build` produces `libvw`, its headers, shaders and the `vw` Lua frontend; `nix run` starts the frontend.

## Without Nix

You need a C++23 compiler (clang is what vw is developed with), CMake, Ninja, `pkg-config`, `glslc` (shaderc) and `wayland-scanner`, plus:

| Library | Used for |
| --- | --- |
| Vulkan loader and headers | rendering |
| freetype, fontconfig, harfbuzz, pango(cairo) | text |
| librsvg, libwebp, cairo | SVG, WebP and canvas images |
| gio, gtk3 | D-Bus services, theme icons |
| pipewire | audio |
| libxkbcommon | keyboards |
| pam | lock screens |
| quickjs | JavaScript canvases |
| nlohmann_json | compositor IPC |
| Lua 5.4 | only with `VW_WITH_LUA` |

## Build options

| Option | Default | Effect |
| --- | --- | --- |
| `VW_BUILD_EXAMPLES` | `ON` | builds `vw-example-bar`, `-launcher`, `-status`, `-tasks` |
| `VW_WITH_LUA` | `ON` | builds the deprecated `vw` Lua frontend |
| `BUILD_TESTING` | CTest default | adds the frontend's smoke tests |

## Use it from your project

Install it, or point your project at a checkout:

```cmake
find_package(vw 0.1 REQUIRED)          # an installed libvw
# or: add_subdirectory(path/to/vw vw EXCLUDE_FROM_ALL)
target_link_libraries(my-shell PRIVATE vw::vw)
```

Only `nlohmann_json` leaks into your build; Wayland, Vulkan and the toolkit libraries are private. vw's own shaders are embedded in the library, so there is nothing to install or find at runtime. The package also brings `vw-shaderc` and `vw_compile_shader()` for [your own shaders](/guides/shaders/).
