---
title: Build and tooling
description: Turning Facet on, compiling .facet files from CMake or Nix, the vw-facetc command, and editor highlighting.
---

## Turning it on

Facet is off unless you ask for it:

```sh
cmake -S . -B build -G Ninja -DVW_WITH_FACET=ON
```

That builds `vw-facetc` (exported as `vw::facetc`) and installs Facet's runtime header, `<vw/facet.hpp>`. Without the option nothing of Facet is built or installed, and vw is unchanged.

## CMake

A vw built with Facet gives `facet_compile()` through `find_package(vw)`:

```cmake
find_package(vw REQUIRED)
facet_compile(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/views.facet.hpp" SOURCE views.facet)
add_executable(my-shell main.cpp "${CMAKE_CURRENT_BINARY_DIR}/views.facet.hpp")
target_include_directories(my-shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(my-shell PRIVATE vw::vw)
```

| Argument | Meaning |
| --- | --- |
| `OUTPUT`, `SOURCE` | the header to write and the `.facet` to read |
| `NAMESPACE` | where the views go; `views` by default |
| `MODULE` | where elements written without a module come from (see [C++ in Facet](/facet/cpp/#component-modules)) |

## Nix

`vw.lib.mkShell` takes `facet = true;`, which builds against vw with Facet (still linked into your binary):

```nix
packages.${system}.default = vw.lib.mkShell {
  inherit system;
  pname = "my-shell";
  src = ./.;
  facet = true;
};
```

`packages.<system>.vw-facet` is vw with Facet on its own.

## vw-facetc

```sh
vw-facetc views.facet -o views.facet.hpp [--namespace NAME] [--module PATH]
```

Without `-o` the header goes to standard output, which is handy for reading what a view becomes. Errors print as `file:line:column: error: …` with the line and a caret.

## Editors

`facet/editors/micro/facet.yaml` highlights Facet in micro: copy it to `~/.config/micro/syntax/`.
