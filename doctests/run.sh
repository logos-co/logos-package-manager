#!/usr/bin/env bash
#
# Execute every lgpm doc-test end-to-end and regenerate its Markdown.
#
# The runner is the shared `doctest` CLI
# (https://github.com/logos-co/logos-doctest), invoked directly via its flake.
# For each *.test.yaml in this directory, `doctest run` executes every command in
# a temp directory (building lgpm, packaging/installing .lgx files, querying them)
# and asserts on the output; `doctest generate` renders the same spec to Markdown
# under outputs/ (gitignored — the *.test.yaml spec is the source of truth).
#
# To run against a local logos-doctest checkout instead of the published flake,
# set DOCTEST, e.g.:  DOCTEST="nix run path:../../logos-doctest --" ./run.sh
#
set -euo pipefail

# Run from this doctests/ directory regardless of where the script is invoked from.
cd "$(dirname "$0")"

# The doctest CLI. Override by exporting DOCTEST (space-separated command).
read -r -a DOCTEST <<< "${DOCTEST:-nix run github:logos-co/logos-doctest --}"

mkdir -p outputs

for spec in *.test.yaml; do
  name="$(basename "${spec%.test.yaml}")"
  echo "==> Running ${spec}"
  "${DOCTEST[@]}" run "${spec}" --verbose --continue-on-fail

  echo "==> Generating outputs/${name}.md"
  "${DOCTEST[@]}" generate "${spec}" -o "outputs/${name}.md"
done

echo "==> Done. Rendered docs are in doctests/outputs/"
