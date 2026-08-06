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
    # Windows adds a third spelling -- libpackage_manager_lib.dll plus its
    # .dll.a import library, which consumers must link against. Every pattern
    # carries a wildcard so nullglob can drop the ones that do not match; a
    # literal path with no wildcard would survive and break the copy.
    shopt -s nullglob
    libs=(lib/libpackage_manager_lib*.dylib lib/libpackage_manager_lib*.so \
          lib/libpackage_manager_lib*.dll lib/libpackage_manager_lib*.dll.a)
    if [ ''${#libs[@]} -eq 0 ]; then
      echo "Error: No library file found" >&2
      ls -la lib >&2 || true
      exit 1
    fi
    cp "''${libs[@]}" $out/lib/

    # Bundle liblgx library alongside so it can be found at runtime
    # liblgx.dll lives in bin/, not lib/: CMake installs Windows RUNTIME
    # artifacts there. Missing it used to be a WARNING, which on Windows would
    # have produced a package that links but cannot start.
    lgxlibs=(${logosPackageLib}/lib/liblgx*.dylib ${logosPackageLib}/lib/liblgx*.so \
             ${logosPackageLib}/bin/liblgx*.dll ${logosPackageLib}/lib/liblgx*.dll.a)
    if [ ''${#lgxlibs[@]} -eq 0 ]; then
      echo "Error: no liblgx runtime found under ${logosPackageLib}" >&2
      ls -la ${logosPackageLib}/lib ${logosPackageLib}/bin >&2 || true
      exit 1
    fi
    cp "''${lgxlibs[@]}" $out/lib/

    # Copy header files
    cp ${src}/src/package_manager_lib.h $out/include/
    cp ${src}/src/lgpm.h $out/include/

    # Copy lgx.h from logos-package so downstream consumers can use the lgx C API
    if [ -f ${logosPackageLib}/include/lgx.h ]; then
      cp ${logosPackageLib}/include/lgx.h $out/include/
    fi

    runHook postInstall
  '';
}
