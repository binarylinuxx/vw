{
  description = "A vw widget";

  inputs.vw.url = "github:binarylinuxx/vw";

  outputs = { self, vw }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = vw.inputs.nixpkgs.lib.genAttrs systems;
    in {
      # CMake, clang, glslc and vw (linked into the binary) come with mkShell;
      # list your own libraries in shellDeps and build tools in nativeShellDeps.
      packages = forAllSystems (system: {
        default = vw.lib.mkShell {
          inherit system;
          pname = "my-widget";
          src = ./.;
          shellDeps = [ ];
        };
      });

      devShells = forAllSystems (system: {
        default = self.packages.${system}.default.devShell;
      });
    };
}
