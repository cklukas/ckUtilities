#!/usr/bin/env bash
# Build a reproducible ckVision SDK for a ckUtilities CI job.
set -euo pipefail

source_dir=${1:?usage: build_ckvision_sdk.sh <ckvision-source> <install-prefix> [build-dir]}
install_prefix=${2:?usage: build_ckvision_sdk.sh <ckvision-source> <install-prefix> [build-dir]}
build_dir=${3:-"${install_prefix}.build"}

export CMAKE_BUILD_PARALLEL_LEVEL=1 CTEST_PARALLEL_LEVEL=1 MAKEFLAGS=-j1 ARCHIVE_BUILD_JOBS=1
[[ ! -e "$build_dir" && ! -e "$install_prefix" ]] || { echo 'Refuse reused SDK/build output; preserve prior owners and choose fresh paths' >&2; exit 1; }
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF \
  -DCKVISION_BUILD_EXAMPLES=OFF \
  -DCKVISION_BUILD_TESTING=OFF \
  -DCKVISION_BUILD_FUZZERS=OFF \
  -DCMAKE_INSTALL_PREFIX="$install_prefix"
cmake --build "$build_dir" --parallel 1
cmake --install "$build_dir"
