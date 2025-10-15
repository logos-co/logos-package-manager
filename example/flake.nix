{
  description = "Package Manager plugin_loader example";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        pkgs = import nixpkgs { inherit system; };
      });
    in
    {
      packages = forAllSystems ({ pkgs }: {
        default = pkgs.stdenv.mkDerivation rec {
          pname = "plugin_loader";
          version = "1.0.0";

          src = ./.;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            pkgs.qt6.wrapQtAppsNoGuiHook
          ];

          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
          ];

          cmakeFlags = [
            "-GNinja"
          ];

          # CMakeLists.txt doesn't define an install() rule, so install manually
          installPhase = ''
            mkdir -p "$out/bin"
            cp -v plugin_loader "$out/bin/"
          '';

          meta = with pkgs.lib; {
            description = "Example Qt-based plugin loader for Logos Package Manager";
            platforms = platforms.unix;
          };
        };
      });

      apps = forAllSystems ({ pkgs }: {
        # Raw plugin_loader so you can pass a custom --path
        default = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.default}/bin/plugin_loader";
        };
      });

      devShells = forAllSystems ({ pkgs }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
          ];
          shellHook = ''
            echo "Package Manager plugin_loader development shell"
          '';
        };
      });
    };
}


