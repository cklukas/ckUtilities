#!/usr/bin/env bash
# Regenerate the README SVGs from real ckUtilities + ckVision rendering.
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${CKTOOLS_DOCS_BUILD_DIR:-$project_root/build/docs}"
ckvision_source="${CKTOOLS_CKVISION_SOURCE_DIR:?CKTOOLS_CKVISION_SOURCE_DIR is required}"
ckvision_prefix="${CKTOOLS_CKVISION_PREFIX:?CKTOOLS_CKVISION_PREFIX is required}"
output_dir="$project_root/docs/generated/screenshots"
expected_revision="4f211569a95ddcd0875d6c5d9778d06d2bf74fec"
actual_revision="$(git -C "$ckvision_source" rev-parse HEAD)"

if [[ "$actual_revision" != "$expected_revision" ]]; then
  echo "Documentation captures require ckVision $expected_revision, not $actual_revision" >&2
  exit 1
fi

cmake -S "$project_root" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$ckvision_prefix" \
  -DCKTOOLS_CKVISION_PREFIX="$ckvision_prefix" \
  -DCKTOOLS_BUILD_DOCUMENTATION_CAPTURES=ON \
  -DCKTOOLS_CKVISION_SOURCE_DIR="$ckvision_source"
cmake --build "$build_dir" --target capture_ckutilities_screenshots --parallel
mkdir -p "$output_dir"
"$build_dir/tools/docgen/capture_ckutilities_screenshots" "$output_dir"
