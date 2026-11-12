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
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      packages = forAllSystems (system:
        let pkgs = nixpkgs.legacyPackages.${system};
        in {
          default = pkgs.clangStdenv.mkDerivation {
            pname = "vw";
            version = "0.1.0";
            src = pkgs.lib.cleanSourceWith {
              src = self;
              filter = path: type:
                let name = baseNameOf path;
                 in name != "build" && name != "result" &&
                    name != "node_modules" && name != "dist" && name != ".astro" &&
                   !(pkgs.lib.hasSuffix ".ppm" name);
            };
            nativeBuildInputs = with pkgs; [ cmake ninja pkg-config shaderc wayland-scanner ];
            buildInputs = with pkgs; [ cairo fontconfig freetype glib sysprof gtk3 harfbuzz librsvg libwebp libxml2 lua5_4 nlohmann_json pam pango pipewire quickjs stb vulkan-headers vulkan-loader wayland wayland-protocols libxkbcommon ];
            cmakeFlags = [ "-DZWAYLAND_ROOT=${zwwm}" ];
          };
        });

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/vw";
        };
      });

      devShells = forAllSystems (system:
        let pkgs = nixpkgs.legacyPackages.${system};
        in {
          default = (pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) {
            ZWAYLAND_ROOT = "${zwwm}";
            packages = with pkgs; [
              cmake
              ninja
              pkg-config
              shaderc
              libxml2
              fontconfig
              freetype
              glib
              sysprof # glib-2.0.pc requires sysprof-capture-4
              gtk3
              harfbuzz
              librsvg
              libwebp
              stb
              nlohmann_json
              pam
              pipewire
              quickjs
              lua5_4
              vulkan-headers
              vulkan-loader
              vulkan-tools
              wayland
              wayland-scanner
              wayland-protocols
              libxkbcommon
              cairo
              pango
            ];
          };
        });
    };
}
