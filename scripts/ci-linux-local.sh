#!/usr/bin/env bash
set -euo pipefail

# Use the same base the GH runner is on today
IMG="docker.io/library/ubuntu:24.04"
WORK="/work"

# Optional: cache build dirs between runs on host
mkdir -p build/dev build/pkg

# If your repo needs GH-like envs, export them here
# export GITHUB_WORKSPACE="$PWD"

# Run the Linux leg exactly like in your workflow
container run --rm -it \
  --name ck-ci-ubuntu \
  --volume "$PWD":"$WORK" -w "$WORK" \
  "$IMG" bash -lc '
    set -euo pipefail
    export DEBIAN_FRONTEND=noninteractive

    apt-get update

    # ---- IMPORTANT: ubuntu-24.04 package names ----
    # * Install libncursesw6-dev for wide-character headers/libncursesw.so.
    # * Keep: libncurses-dev, libncursesw6, libtinfo-dev, libgpm-dev.
    # * Add: git, cmake, build-essential (GH image has them preinstalled).
    apt-get install -y \
      git ca-certificates curl build-essential cmake ninja-build \
      libncurses-dev libncursesw6 libncursesw6-dev libtinfo-dev libgpm-dev pkg-config \
      libcurl4-openssl-dev

    # Configure / Build / Test (from your ci.yml)
    rm -rf build/dev build/pkg
    cmake --preset dev
    cmake --build build/dev
    ctest --test-dir build/dev --output-on-failure

    # (Optional) Also exercise packaging like your package-linux job:
    cmake --preset pkg
    cmake --build build/pkg -t package
    ls -R1 build/pkg || true
  '
