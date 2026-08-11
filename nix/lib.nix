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

    # Windows only: liblgx's OWN runtime dependencies, not just liblgx itself.
    #
    # ELF and Mach-O consumers never need this -- the copied liblgx carries an
    # RPATH / install-name back to the store paths its dependencies live in.
    # PE has no rpath: an import table holds a DLL BASE NAME, resolved from the
    # loading executable's directory. So a consumer can only stage what is
    # present HERE, and anything left behind is unreachable later.
    #
    # ${logosPackageLib}/bin is exactly the right source: nixpkgs'
    # win-dll-link.sh has already walked lgx.exe's import graph and staged the
    # complete transitive closure there (libsodium-26.dll, the ICU pair,
    # zlib1.dll, ...). Copying the DLLs from it is transitive by construction
    # rather than a hand-maintained list that silently rots.
    #
    # Measured failure this prevents: with liblgx.dll present but
    # libsodium-26.dll missing, logosctl.exe exited 53 with NO OUTPUT AT ALL --
    # the loader gives up before main() and names nothing.
    #
    # Explicit `for` + `-f`, not a nullglob array: nullglob only drops patterns
    # that CONTAIN a wildcard, so a fully interpolated literal path survives
    # into the array and any guard over it passes vacuously.
    if [ -d "${logosPackageLib}/bin" ]; then
      for dep in ${logosPackageLib}/bin/*.dll; do
        [ -f "$dep" ] || continue
        # Do not clobber the liblgx.dll just copied above.
        [ -e "$out/lib/$(basename "$dep")" ] && continue
        cp -L "$dep" $out/lib/
      done
    fi

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
