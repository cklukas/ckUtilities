#!/usr/bin/env bash
set -euo pipefail

sudo apt-get update
sudo apt-get install -y --no-install-recommends ca-certificates curl gnupg lsb-release software-properties-common

KITWARE_KEYRING="/usr/share/keyrings/kitware-archive-keyring.gpg"
KITWARE_LIST="/etc/apt/sources.list.d/kitware.list"

if [[ ! -f "${KITWARE_KEYRING}" ]]; then
  curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc | sudo gpg --dearmor -o "${KITWARE_KEYRING}"
fi

if [[ ! -f "${KITWARE_LIST}" ]]; then
  echo "deb [signed-by=${KITWARE_KEYRING}] https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" | sudo tee "${KITWARE_LIST}" >/dev/null
fi

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  cmake \
  ninja-build \
  clang-15 \
  clang-tidy-15 \
  lld-15 \
  llvm-15-dev \
  git \
  pandoc \
  texinfo \
  dpkg-dev \
  rpm \
  ccache \
  lcov

sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-15 150
sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-15 150
sudo update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-15 150

CMAKE_VERSION=$(cmake --version | head -n1 | awk '{print $3}')
if dpkg --compare-versions "${CMAKE_VERSION}" lt 3.27; then
  echo "CMake 3.27+ is required, but ${CMAKE_VERSION} is installed." >&2
  exit 1
fi

cmake --preset dev || true
