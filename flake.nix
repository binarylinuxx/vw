{
  description = "vw: a C++ Wayland widget library with a Vulkan renderer";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    zwwm = {
      url = "github:zwwmWM/zwwm";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, zwwm }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      pkgsFor = system: import nixpkgs {
        inherit system;
        overlays = [ self.overlays.default ];
      };
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (pkgsFor system));
      src = nixpkgs.lib.cleanSourceWith {
        src = self;
        filter = path: type:
          let name = baseNameOf path;
          in name != "build" && name != "result" && name != "node_modules" && name != "dist" &&
             name != ".astro" && name != "vw-docs" &&
             !(nixpkgs.lib.hasSuffix ".ppm" name);
      };
    in {
      # Adds pkgs.vw; use it from another flake with
      #   import nixpkgs { inherit system; overlays = [ vw.overlays.default ]; }
      overlays.default = final: prev:
        let
          vw = facet: final.callPackage ./nix/package.nix { inherit src zwwm; withFacet = facet; };
          # libvw.a: links into the program, so a shell ships as one binary.
          # Its libraries come along for the program to link.
          static = package: package.overrideAttrs (old: {
            pname = old.pname + "-static";
            cmakeFlags = old.cmakeFlags ++ [ "-DVW_STATIC=ON" ];
            propagatedBuildInputs = old.propagatedBuildInputs ++ old.buildInputs;
            propagatedNativeBuildInputs = (old.propagatedNativeBuildInputs or [ ]) ++ [ final.pkg-config ];
          });
          facet = package: package.overrideAttrs (old: { pname = old.pname + "-facet"; });
        in {
          vw = vw false;
          vw-static = static (vw false);
          # With Facet: vw-facetc, <vw/facet.hpp> and facet_compile().
          vw-facet = facet (vw true);
          vw-static-facet = facet (static (vw true));
        };

      # A shell (any CMake program on vw) as a package, vw linked in statically:
      #
      #   packages.default = vw.lib.mkShell {
      #     inherit system;
      #     pname = "my-shell";
      #     src = ./.;
      #     shellDeps = [ pkgs.libnotify ];   # libraries, on top of vw's
      #     nativeShellDeps = [ pkgs.python3 ];   # build tools, on top of cmake, ninja, glslc
      #   };
      #   devShells.default = self.packages.${system}.default.devShell;
      #
      # Every other attribute (version, cmakeFlags, meta, postInstall, ...) goes
      # to mkDerivation.
      #
      # facet = true builds against vw with Facet: write views in .facet and
      # facet_compile() them in CMake.
      lib.mkShell = { system, pname, src, version ? "0.1.0", shellDeps ? [ ], nativeShellDeps ? [ ], facet ? false, ... }@args:
        let
          pkgs = pkgsFor system;
          rest = removeAttrs args [ "system" "shellDeps" "nativeShellDeps" "facet" ];
          package = pkgs.clangStdenv.mkDerivation (rest // {
            inherit pname version;
            # A local build tree's CMakeCache.txt would take over the build.
            src = if !(nixpkgs.lib.isDerivation src) then nixpkgs.lib.cleanSourceWith {
              inherit src;
              filter = path: type: !(builtins.elem (baseNameOf path) [ "build" "result" ]);
            } else src;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.shaderc ] ++ nativeShellDeps
              ++ (args.nativeBuildInputs or [ ]);
            buildInputs = [ (if facet then pkgs.vw-static-facet else pkgs.vw-static) ] ++ shellDeps
              ++ (args.buildInputs or [ ]);
            meta = { mainProgram = pname; platforms = nixpkgs.lib.platforms.linux; } // (args.meta or { });
            passthru = (args.passthru or { }) // {
              # Everything the package builds with, for working on it by hand.
              devShell = (pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) {
                inputsFrom = [ package ];
              };
            };
          });
        in package;

      packages = forAllSystems (pkgs: {
        default = pkgs.vw;
        vw = pkgs.vw;
        vw-static = pkgs.vw-static;
        vw-facet = pkgs.vw-facet;
      });

      apps = forAllSystems (pkgs: {
        default = {
          type = "app";
          program = nixpkgs.lib.getExe pkgs.vw;
          meta.description = "vw's Lua frontend";
        };
      });

      # For working on vw itself: everything the package builds with.
      devShells = forAllSystems (pkgs: {
        default = (pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) {
          inputsFrom = [ pkgs.vw ];
          packages = [ pkgs.vulkan-tools ];
          ZWAYLAND_ROOT = "${zwwm}";
        };
      });

      # nix flake init -t github:binarylinuxx/vw
      templates.default = {
        path = ./templates/default;
        description = "A C++ project using vw, built with CMake";
      };
    };
}
