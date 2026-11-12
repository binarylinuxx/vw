{
  description = "A vw widget";

  inputs = {
    vw.url = "github:binarylinuxx/vw";
    nixpkgs.follows = "vw/nixpkgs";
  };

  outputs = { self, nixpkgs, vw }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs {
        inherit system;
        overlays = [ vw.overlays.default ];
      }));
    in {
      packages = forAllSystems (pkgs: {
        default = pkgs.clangStdenv.mkDerivation {
          pname = "my-widget";
          version = "0.1.0";
          src = ./.;
          # shaderc only matters if you compile shaders with vw_compile_shader.
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.shaderc ];
          buildInputs = [ pkgs.vw ];
          meta.mainProgram = "my-widget";
        };
      });

      devShells = forAllSystems (pkgs: {
        default = (pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
        };
      });
    };
}
