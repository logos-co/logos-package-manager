# Common build configuration shared across all packages
{ pkgs, logosPackageLib, portableBuild ? false }:

{
  pname = "logos-package-manager";
  version = "1.0.0";

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
