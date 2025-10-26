#!/usr/bin/env bash
set -euo pipefail

#!/usr/bin/env bash
set -euo pipefail

# Minimal helper to configure + build ck-chat using the dev preset.
# Usage: scripts/build_ck_chat.sh [build-dir]

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_BUILD_DIR="${ROOT_DIR}/build/dev"
BUILD_DIR="${1:-${DEFAULT_BUILD_DIR}}"

if [ "${BUILD_DIR}" = "${DEFAULT_BUILD_DIR}" ]; then
    if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
        cmake --preset dev
    fi
    "${ROOT_DIR}/scripts/apply_patches.sh" "${BUILD_DIR}"
    cmake --build "${BUILD_DIR}" --target ckchat
else
    if [ ! -d "${BUILD_DIR}" ] || [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
        cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
    fi
    "${ROOT_DIR}/scripts/apply_patches.sh" "${BUILD_DIR}"
    cmake --build "${BUILD_DIR}" --target ckchat
fi
