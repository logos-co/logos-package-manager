# Builds and runs logos-package-manager tests
{ pkgs, common, src, logosPackageLib }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;

  inherit src;
  inherit (common) nativeBuildInputs meta env;

  buildInputs = common.buildInputs ++ [ pkgs.gtest ];

  configurePhase = ''
    runHook preConfigure

    cmake -S . -B build \
      -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DLGX_ROOT=${logosPackageLib} \
      -DLGPM_BUILD_TESTS=ON \
      ${pkgs.lib.optionalString (builtins.elem "-DLGPM_PORTABLE_BUILD=ON" common.cmakeFlags) "-DLGPM_PORTABLE_BUILD=ON"}

    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    cmake --build build
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    cd build
    ctest --output-on-failure
    cd ..
    runHook postCheck
  '';

  installPhase = ''
    mkdir -p $out
    echo "Tests passed" > $out/test-results.txt
  '';
}
