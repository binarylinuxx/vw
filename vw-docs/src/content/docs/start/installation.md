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

To package a shell from a flake you already have, hand it to `vw.lib.mkShell`. It builds your `CMakeLists.txt` with clang, Ninja and glslc, links vw in statically so the package is your shell alone, and drops a local `build/` from the source:

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
        shellDeps = [ pkgs.libnotify ];        # libraries, on top of vw's
        nativeShellDeps = [ pkgs.python3 ];    # build tools, on top of cmake, ninja, glslc
      };
      devShells.${system}.default = self.packages.${system}.default.devShell;
    };
}
```

Every other attribute (`version`, `cmakeFlags`, `postInstall`, `meta`, …) goes straight to `mkDerivation`. `.devShell` is a clang shell with everything the package builds with, for `cmake` by hand.

Facet, the declarative view language that compiles to vw, is a separate and optional project. Pass its package as `facet = …;` (or use its flake's own `lib.mkShell`, which does that) to get `facetc` and `facet_compile()` in the build; without it nothing of Facet is involved.

For full control, take the overlay instead: it adds `pkgs.vw` and `pkgs.vw-static` next to the rest of nixpkgs, and `find_package(vw)` works as below.

```nix
pkgs = import nixpkgs { inherit system; overlays = [ vw.overlays.default ]; };
# then: buildInputs = [ pkgs.vw-static ]; nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
```

| Flake output | What it is |
| --- | --- |
| `overlays.default` | adds `pkgs.vw` and `pkgs.vw-static` |
| `packages.<system>.default`, `.vw` | the library, headers, CMake package, `vw-shaderc`, examples |
| `packages.<system>.vw-static` | the same with `libvw.a`; overlay name `pkgs.vw-static` |
| `devShells.<system>.default` | everything needed to work on vw itself |
| `lib.mkShell` | a CMake shell as a package, vw linked in; `.devShell` to work on it |
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
| `VW_STATIC` | `OFF` | builds `libvw.a`, which links into your program: a shell ships as one binary with nothing of vw to install |
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
