{
  description = "Logos Package Manager - Local package management library and CLI";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-package.url = "github:logos-co/logos-package";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
  };

  outputs = { self, nixpkgs, logos-nix, logos-package, nix-bundle-dir, nix-bundle-appimage }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      # Build info baked into the lgpm binary so `--version` reports the release
      # version, this repo's commit, and the locked commits of the flake inputs.
      # `revOf` yields the input's locked rev, a "<sha>-dirty" marker for a dirty
      # checkout, or "dirty" for a path override.
      revOf = input: input.rev or input.dirtyRev or "dirty";
      buildInfo = {
        # VERSION is only present on release branches. On master (pre-release CI
        # builds) there is no VERSION file, so fall back to a "pre-release-{sha7}"
        # string derived from self.rev. Dirty local builds lack self.rev and get
        # an empty string, which the CLI renders as "dev".
        version = if builtins.pathExists ./VERSION
          then nixpkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION)
          else if (self ? rev) then "pre-release-${builtins.substring 0 7 self.rev}" else "";
        commit = revOf self;
        commits = [
          { name = "logos-package"; commit = revOf logos-package; }
          { name = "logos-nix"; commit = revOf logos-nix; }
          { name = "nix-bundle-dir"; commit = revOf nix-bundle-dir; }
          { name = "nix-bundle-appimage"; commit = revOf nix-bundle-appimage; }
        ];
      };
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        logosPackageLib = logos-package.packages.${system}.lib;
        dirBundler = nix-bundle-dir.bundlers.${system}.permissive;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, logosPackageLib, dirBundler }:
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
          cli = import ./nix/cli.nix { inherit pkgs common src logosPackageLib buildInfo; };

          # CLI package (portable)
          cliPortable = import ./nix/cli.nix { inherit pkgs src logosPackageLib buildInfo; common = commonPortable; };

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
