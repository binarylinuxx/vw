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
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs {
        inherit system;
        overlays = [ self.overlays.default ];
      }));
      src = nixpkgs.lib.cleanSourceWith {
        src = self;
        filter = path: type:
          let name = baseNameOf path;
          in name != "build" && name != "result" && name != "node_modules" && name != "dist" &&
             name != ".astro" && name != "vw-docs" && name != "templates" &&
             !(nixpkgs.lib.hasSuffix ".ppm" name);
      };
    in {
      # Adds pkgs.vw; use it from another flake with
      #   import nixpkgs { inherit system; overlays = [ vw.overlays.default ]; }
      overlays.default = final: prev: {
        vw = final.callPackage ./nix/package.nix { inherit src zwwm; };
      };

      packages = forAllSystems (pkgs: {
        default = pkgs.vw;
        vw = pkgs.vw;
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
