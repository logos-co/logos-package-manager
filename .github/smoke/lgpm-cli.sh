#!/usr/bin/env bash
# The EXERCISE half of doctests/lgpm-cli.test.yaml, as a Windows smoke script.
#
# That spec has 13 `run:` steps and exactly ONE invokes nix. The other 12 only
# drive the built binary, and the nix one needs no translation either: it IS
# `targets: cli` in the caller. So the doc-tested install lifecycle runs on real
# Windows without doctest.py, which cannot run there, having to change.
#
# `set -e` is redundant -- the smoke action already runs this with
# `bash -euo pipefail` -- and is kept so the file behaves identically when run
# by hand against a native build, which is how it was developed:
#     LGPM=cli/bin/lgpm bash .github/smoke/lgpm-cli.sh
# from a directory containing cli/bin/lgpm.
set -euo pipefail
LGPM="${LGPM:-cli/bin/lgpm.exe}"

command -v tar >/dev/null || {
  echo "::error::tar is not on PATH, so the packages cannot be built."
  echo "::error::This is a runner-image fact, not a defect in this repo."
  exit 1
}

# --format=ustar, EVERYWHERE, and it is not cosmetic.
#
# bsdtar's default is "restricted pax": it emits a PaxHeader entry whenever a
# file needs something ustar cannot express -- and a SUB-SECOND MTIME is enough,
# which every freshly-written file here has. lgpm's tar reader cannot parse
# those entries, and the package fails to load.
#
# MEASURED on macOS (bsdtar 3.5.3), building the same tree three ways:
#   pax headers present, no AppleDouble entries -> lgpm install FAILS
#   pax headers present, AppleDouble entries too -> lgpm install FAILS
#   --format=ustar, no pax headers              -> lgpm install OK
# so the pax entries are the cause on their own; the macOS-only ._* files are
# incidental.
#
# WHY THAT MATTERS HERE rather than being a macOS quirk: Windows ships bsdtar as
# C:\Windows\System32\tar.exe, and which tar an MSYS bash resolves is a PATH
# question nobody has answered on the runner. GNU tar accepts --format=ustar and
# emits no pax entries either (verified, GNU tar 1.35), so naming the format
# costs nothing and removes the question.
TAR=(tar --format=ustar)

# --- Confirm it runs ---------------------------------------------------------
# Also the only check that the 18 staged DLLs actually resolve: a missing one
# kills the process before main() with no output at all.
run "$LGPM" --version | tee version.txt
grep -qi "lgpm" version.txt

# --- The platform variant ----------------------------------------------------
# The spec probes with `uname` and has no Windows arm, exiting 1 there. Here the
# variant is not unknown -- we built FOR this target -- so it is stated. A dev
# build of lgpm computes `<platform>-dev`, hence the suffix; `windows-x86_64` is
# the name lgpm itself derives and nix-bundle-lgx emits.
VARIANT=windows-x86_64-dev
EXT=dll

# --- Build the core module package -------------------------------------------
# The manifest is written with the variant already substituted, rather than
# stamped in afterwards with `sed -i` as the spec does: -i takes a mandatory
# suffix on BSD sed and rejects one on GNU sed, and this script has to run
# unmodified on two platforms.
mkdir -p "build/greeter/variants/$VARIANT"
cat > build/greeter/manifest.json <<JSON
{
  "manifestVersion": "0.2.0",
  "name": "greeter",
  "version": "1.0.0",
  "description": "Says hello",
  "author": "Logos",
  "type": "core",
  "category": "demo",
  "icon": "",
  "dependencies": [],
  "main": { "$VARIANT": "greeter.$EXT" }
}
JSON
echo 'stub library' > "build/greeter/variants/$VARIANT/greeter.$EXT"
"${TAR[@]}" -C build/greeter -czf greeter.lgx manifest.json variants

tar -tzf greeter.lgx > layout.txt
grep -q "manifest.json"                        layout.txt
grep -q "variants/$VARIANT/greeter.$EXT"       layout.txt

# --- Build the UI plugin package, which depends on the core module -----------
mkdir -p "build/greeter_ui/variants/$VARIANT"
cat > build/greeter_ui/manifest.json <<JSON
{
  "manifestVersion": "0.2.0",
  "name": "greeter_ui",
  "version": "1.0.0",
  "description": "A face for greeter",
  "author": "Logos",
  "type": "ui",
  "category": "demo",
  "icon": "",
  "dependencies": ["greeter"],
  "main": { "$VARIANT": "greeter_ui.$EXT" }
}
JSON
echo 'stub library' > "build/greeter_ui/variants/$VARIANT/greeter_ui.$EXT"
"${TAR[@]}" -C build/greeter_ui -czf greeter_ui.lgx manifest.json variants

# --- Install both ------------------------------------------------------------
DIRS=(--modules-dir ./modules --ui-plugins-dir ./plugins)
run "$LGPM" "${DIRS[@]}" --allow-unsigned install --file ./greeter.lgx    | tee i1.txt
grep -q "Installed to:" i1.txt
run "$LGPM" "${DIRS[@]}" --allow-unsigned install --file ./greeter_ui.lgx | tee i2.txt
grep -q "Installed to:" i2.txt

# The install is what actually exercises the variant name: lgpm computes this
# machine's variant and fail-closes if the package carries no matching payload.
# On Windows that refusal is the thing standing between a Windows package and
# being installed as a Linux one, so a green install here is the real assertion.

# --- Read the index back -----------------------------------------------------
run "$LGPM" "${DIRS[@]}" list | tee list.txt
grep -q "greeter"    list.txt
grep -q "greeter_ui" list.txt

run "$LGPM" "${DIRS[@]}" info greeter | tee info.txt
grep -q "Name: greeter"   info.txt
grep -q "Version: 1.0.0"  info.txt
grep -q "Type: core"      info.txt

run "$LGPM" "${DIRS[@]}" info greeter_ui --json | tee info.json
grep -q '"name": "greeter_ui"' info.json
grep -q '"version": "1.0.0"'   info.json

# --- The dependency graph ----------------------------------------------------
run "$LGPM" "${DIRS[@]}" deps greeter_ui | tee deps.txt
grep -q "greeter" deps.txt
run "$LGPM" "${DIRS[@]}" dependents greeter | tee dependents.txt
grep -q "greeter_ui" dependents.txt

echo "OK: lgpm-cli exercise half passed"
