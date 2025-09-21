#!/usr/bin/env bash
set -euo pipefail

build_dir=${1:-build/dev}
ctest --test-dir "${build_dir}" --output-on-failure -L integration
