#!/usr/bin/env bash
set -euo pipefail

# Resolve repository root
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

echo "[ci-mac] Working directory: $REPO_ROOT"

# Ensure Homebrew dependencies match CI workflow
BREW_PACKAGES=(ninja ncurses curl)
if command -v brew >/dev/null 2>&1; then
  echo "[ci-mac] Updating Homebrew formulae (override with CI_MAC_SKIP_BREW_UPDATE=1)..."
  if [[ "${CI_MAC_SKIP_BREW_UPDATE:-0}" != "1" ]]; then
    brew update --quiet
  fi
  echo "[ci-mac] Installing required packages: ${BREW_PACKAGES[*]}"
  brew install --quiet "${BREW_PACKAGES[@]}" >/dev/null || true
else
  echo "[ci-mac] Homebrew not found. Please install Homebrew before running this script." >&2
  exit 1
fi

NCURSES_PREFIX="$(brew --prefix ncurses)"
CURL_PREFIX="$(brew --prefix curl)"

# Mirror the environment tweaks from the GitHub Action
export CMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:+$CMAKE_PREFIX_PATH:}$NCURSES_PREFIX"
export CPATH="${CPATH:+$CPATH:}$NCURSES_PREFIX/include"
export LIBRARY_PATH="${LIBRARY_PATH:+$LIBRARY_PATH:}$NCURSES_PREFIX/lib"

export CMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:+$CMAKE_PREFIX_PATH:}$CURL_PREFIX"
export CPATH="${CPATH:+$CPATH:}$CURL_PREFIX/include"
export LIBRARY_PATH="${LIBRARY_PATH:+$LIBRARY_PATH:}$CURL_PREFIX/lib"
export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$CURL_PREFIX/lib/pkgconfig"

# Clean and recreate build directories to ensure parity with CI runs
echo "[ci-mac] Resetting build directories..."
rm -rf build/dev build/pkg

echo "[ci-mac] Configuring debug build..."
cmake --preset dev

echo "[ci-mac] Building all targets..."
cmake --build build/dev

echo "[ci-mac] Running unit tests..."
ctest --test-dir build/dev --output-on-failure

echo "[ci-mac] Configuring packaging build..."
cmake --preset pkg -DCMAKE_INSTALL_PREFIX=.

echo "[ci-mac] Building macOS package artifacts..."
cmake --build build/pkg -t package

echo "[ci-mac] Package contents:"
ls -R1 build/pkg || true

echo "[ci-mac] Done. Artifacts under build/dev and build/pkg."
