#!/usr/bin/env bash
set -euo pipefail

# Use the same base the GH runner is on today
IMG="docker.io/library/ubuntu:24.04"
WORK="/work"

# Optional: cache build dirs between runs on host
mkdir -p build/dev build/pkg

# If your repo needs GH-like envs, export them here
# export GITHUB_WORKSPACE="$PWD"

container rm -f ck-ci-ubuntu >/dev/null 2>&1 || true

# Run the Linux leg exactly like in your workflow
TTY_FLAGS="-i"
if [ -t 0 ] && [ -t 1 ]; then
  TTY_FLAGS="-it"
fi

container run --rm ${TTY_FLAGS} \
  --name ck-ci-ubuntu \
  --volume "$PWD":"$WORK" -w "$WORK" \
  "$IMG" bash -lc '
    set -euo pipefail
    export DEBIAN_FRONTEND=noninteractive

    apt-get update

    # ---- IMPORTANT: ubuntu-24.04 package names ----
    # * libncurses-dev ships the wide-character headers/libncursesw.so under /usr/lib/$(gcc -print-multiarch).
    # * Keep: libncurses-dev, libncursesw6, libtinfo-dev, libgpm-dev.
    # * Add: git, cmake, build-essential (GH image has them preinstalled).
    apt-get install -y \
      git ca-certificates curl build-essential cmake ninja-build \
      libncurses-dev libncursesw6 libtinfo-dev libgpm-dev pkg-config \
      libcurl4-openssl-dev

    # Ensure multiarch include/lib directories are visible to CMake.
    multiarch="$(gcc -print-multiarch)"
    export LIBRARY_PATH="/usr/lib/${multiarch}${LIBRARY_PATH:+:$LIBRARY_PATH}"
    export CMAKE_LIBRARY_PATH="/usr/lib/${multiarch}${CMAKE_LIBRARY_PATH:+:$CMAKE_LIBRARY_PATH}"
    export CPATH="/usr/include/${multiarch}${CPATH:+:$CPATH}"
    export CMAKE_INCLUDE_PATH="/usr/include/${multiarch}${CMAKE_INCLUDE_PATH:+:$CMAKE_INCLUDE_PATH}"
    export PKG_CONFIG_PATH="/usr/lib/${multiarch}/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-1}"

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
