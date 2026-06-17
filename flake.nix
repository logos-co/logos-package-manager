{
  description = "Logos Package Manager - Local package management library and CLI";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-package.url = "github:logos-co/logos-package";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
    nix-bundle-macos-app = {
      url = "github:logos-co/nix-bundle-macos-app";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.nix-bundle-dir.follows = "nix-bundle-dir";
    };
  };

  outputs = { self, nixpkgs, logos-nix, logos-package, nix-bundle-dir, nix-bundle-appimage, nix-bundle-macos-app }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        logosPackageLib = logos-package.packages.${system}.lib;
        dirBundler = nix-bundle-dir.bundlers.${system}.permissive;
        macosAppBundler = nix-bundle-macos-app.lib.${system}.mkMacOSApp;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, logosPackageLib, dirBundler, macosAppBundler }:
        let
          # Common configuration (dev, default)
          common = import ./nix/default.nix { inherit pkgs logosPackageLib; };
          # Common configuration (portable)
          commonPortable = import ./nix/default.nix { inherit pkgs logosPackageLib; portableBuild = true; };
          src = ./.;

          # Library package (dev)
          lib = import ./nix/lib.nix { inherit pkgs common src logosPackageLib; };

          # Library package (portable)
          libPortable = import ./nix/lib.nix { inherit pkgs src logosPackageLib; common = commonPortable; };

          # CLI package (dev)
          cli = import ./nix/cli.nix { inherit pkgs common src logosPackageLib; };

          # CLI package (portable)
          cliPortable = import ./nix/cli.nix { inherit pkgs src logosPackageLib; common = commonPortable; };

          # Combined package
          combined = pkgs.symlinkJoin {
            name = "logos-package-manager";
            paths = [ lib cli ];
          };
        in
        {
          # Individual outputs
          logos-package-manager-lib = lib;
          logos-package-manager-cli = cli;
          lib = lib;
          lib-portable = libPortable;
          cli = cli;
          cli-portable = cliPortable;

          # Bundle outputs
          cli-bundle-dir = dirBundler cliPortable;
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          cli-appimage = nix-bundle-appimage.lib.${system}.mkAppImage {
            drv = cliPortable;
            name = "lgpm";
            bundle = dirBundler cliPortable;
            desktopFile = ./assets/lgpm.desktop;
            icon = ./assets/lgpm.png;
          };
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isDarwin {
          # macOS .app bundle (ad-hoc signed, notarization-ready structure).
          # Released as a tar.gz; see .github/workflows/release.yml.
          cli-macos-app = macosAppBundler {
            drv = cliPortable;
            name = "lgpm";
            bundle = dirBundler cliPortable;
            icon = ./assets/lgpm.png;
            infoPlist = ./assets/macos/Info.plist.in;
            entitlements = ./assets/macos/lgpm.entitlements;
          };
        } // {
          # Tests
          tests = import ./nix/tests.nix { inherit pkgs common src logosPackageLib; };

          # Default package (combined)
          default = combined;
        }
      );

      checks = forAllSystems ({ pkgs, system, logosPackageLib, ... }:
        let
          common = import ./nix/default.nix { inherit pkgs logosPackageLib; };
          src = ./.;
        in {
          tests = import ./nix/tests.nix { inherit pkgs common src logosPackageLib; };
        }
      );

      devShells = forAllSystems ({ pkgs, logosPackageLib, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.nlohmann_json
            pkgs.zstd
          ];

          shellHook = ''
            export LGX_ROOT="${logosPackageLib}"
            echo "Logos Package Manager development environment"
            echo "LGX_ROOT: $LGX_ROOT"
          '';
        };
      });
    };
}
