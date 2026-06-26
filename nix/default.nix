# Common build configuration shared across all packages
{ pkgs, logosPackageLib, portableBuild ? false }:

{
  pname = "logos-package-manager";
  # VERSION is only present on release branches; dev branches use a placeholder.
  version = if builtins.pathExists ../VERSION
    then pkgs.lib.removeSuffix "\n" (builtins.readFile ../VERSION)
    else "1.0.0-dev";

  # Common native build inputs
  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
  ];

  # Common runtime dependencies
  buildInputs = [
    pkgs.nlohmann_json
    pkgs.zstd
    logosPackageLib
  ];

  # Common CMake flags
  cmakeFlags = [
    "-GNinja"
    "-DLGX_ROOT=${logosPackageLib}"
  ] ++ pkgs.lib.optionals portableBuild [
    "-DLGPM_PORTABLE_BUILD=ON"
  ];

  # Environment variables
  env = {
    LGX_ROOT = "${logosPackageLib}";
  };

  # Metadata
  meta = with pkgs.lib; {
    description = "Logos Package Manager - Local package management library and CLI";
    platforms = platforms.unix;
  };
}
