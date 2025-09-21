#!/usr/bin/env bash
set -euo pipefail

command=${1:-}
case "${command}" in
  format)
    echo "format: clang-format integration to be wired up";;
  tidy)
    echo "tidy: clang-tidy integration to be wired up";;
  quick-build)
    cmake --preset dev
    cmake --build build/dev --target ckjsonview;;
  "")
    echo "usage: $0 {format|tidy|quick-build}" >&2
    exit 1;;
  *)
    echo "unknown command: ${command}" >&2
    exit 1;;
 esac
