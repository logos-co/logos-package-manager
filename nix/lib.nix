# Builds the logos-package-manager library
{ pkgs, common, src, logosPackageLib }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-lib";
  version = common.version;

  inherit src;
  inherit (common) nativeBuildInputs buildInputs cmakeFlags meta env;

  installPhase = ''
    runHook preInstall

    mkdir -p $out/lib $out/include

    # Copy the built library file
    if [ -f lib/libpackage_manager_lib.dylib ]; then
      cp lib/libpackage_manager_lib.dylib $out/lib/
    elif [ -f lib/libpackage_manager_lib.so ]; then
      cp lib/libpackage_manager_lib.so $out/lib/
    else
      echo "Error: No library file found"
      exit 1
    fi

    # Bundle liblgx library alongside so it can be found at runtime
    if [ -f ${logosPackageLib}/lib/liblgx.dylib ]; then
      cp ${logosPackageLib}/lib/liblgx.dylib $out/lib/
    elif [ -f ${logosPackageLib}/lib/liblgx.so ]; then
      cp ${logosPackageLib}/lib/liblgx.so $out/lib/
    else
      echo "Warning: liblgx library not found in ${logosPackageLib}/lib/"
    fi

    # Copy header files
    cp ${src}/src/package_manager_lib.h $out/include/
    cp ${src}/src/lgpm.h $out/include/

    runHook postInstall
  '';
}
