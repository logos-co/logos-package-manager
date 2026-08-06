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
  ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isWindows [
    # WINDOWS_EXPORT_ALL_SYMBOLS builds its .def file by running objdump over
    # the object files. CMake looks up CMAKE_OBJDUMP to do that, nixpkgs does
    # not set it for a cross build (it sets AR/RANLIB/STRIP but not OBJDUMP),
    # and when it is missing CMake skips def-file generation SILENTLY -- the
    # property appears to be honoured and exports nothing. Point it at the
    # target objdump explicitly.
    "-DCMAKE_OBJDUMP=${pkgs.stdenv.cc.bintools.bintools}/bin/${pkgs.stdenv.cc.targetPrefix}objdump"
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
    platforms = platforms.unix ++ platforms.windows;
  };
}
