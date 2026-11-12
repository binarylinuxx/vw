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

## Start a project

The `vw` command writes a new project, named after its directory, and builds it:

```sh
vw --init my-widget             # CMakeLists.txt, main.cpp, .gitignore; cmake configure + build
vw --init-nix my-widget         # the same with flake.nix: git add, nix flake lock, build in its dev shell
./my-widget/build/my-widget
```

`--init` builds with clang and Ninja when they are installed and points CMake at the vw it came with. Neither overwrites an existing file.

## In a Nix flake project

Without `vw` installed, start from the flake template instead; it has the same files:

```sh
mkdir my-widget && cd my-widget
nix flake init -t github:binarylinuxx/vw
git init && git add -A          # flakes only see tracked files
nix run                         # builds and starts it
nix develop                     # or work on it with cmake by hand
```

To add vw to a flake you already have, take its overlay, which puts `pkgs.vw` next to the rest of nixpkgs:

```nix
{
  inputs = {
    vw.url = "github:binarylinuxx/vw";
    nixpkgs.follows = "vw/nixpkgs";
  };

  outputs = { self, nixpkgs, vw }:
    let pkgs = import nixpkgs { system = "x86_64-linux"; overlays = [ vw.overlays.default ]; };
    in {
      packages.x86_64-linux.default = pkgs.clangStdenv.mkDerivation {
        pname = "my-widget";
        version = "0.1.0";
        src = ./.;
        nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
        buildInputs = [ pkgs.vw ];
      };
    };
}
```

`find_package(vw)` then works as below. Add `pkgs.shaderc` to `nativeBuildInputs` if you compile shaders with `vw_compile_shader()`. `vw.packages.<system>.default` is the same package without the overlay.

| Flake output | What it is |
| --- | --- |
| `overlays.default` | adds `pkgs.vw` |
| `packages.<system>.default`, `.vw` | the library, headers, CMake package, `vw-shaderc`, examples |
| `devShells.<system>.default` | everything needed to work on vw itself |
| `templates.default` | a CMake project using vw |

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
| `VW_BUILD_EXAMPLES` | `ON` | builds the `vw-example-*` programs from `examples/cpp` |
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
